#!/usr/bin/env python3
"""
validate.py — TinyGuard Milestone 6 Validation
================================================
Sends crafted UDP heartbeat packets to the monitor to trigger each
detection scenario without needing to physically manipulate hardware.

Run this from your laptop while the monitor is running and has
completed its learning phase (5 minutes after first real heartbeat).

Usage:
    python3 validate.py <scenario>

Scenarios:
    normal          — 30 normal packets (baseline verification)
    rssi            — RSSI spike: 10 normal then 5 anomalous (-95 dBm)
    reconnect       — Reconnect spike: rapidly incrementing reconnect counter
    stream          — Stream anomaly: viewer count spike to 50
    heartbeat_stop  — Stop sending for 35s to trigger CRITICAL timeout
    all             — Run all scenarios in sequence with pauses between

The monitor's dashboard at http://192.168.137.20/ should show alerts
in real-time as each scenario runs.

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
    sock.sendto(packet.encode(), (MONITOR_IP, 500))
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


def scenario_all():
    """Run all scenarios in sequence."""
    print("=" * 60)
    print("TinyGuard Validation — Full Suite")
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

    print("\n" + "=" * 60)
    print("Validation complete. Check dashboard for alert history.")
    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(description="TinyGuard validation injector")
    parser.add_argument("scenario",
                        choices=["normal","rssi","reconnect","stream",
                                 "heartbeat_stop","all"],
                        help="Scenario to run")
    args = parser.parse_args()

    scenarios = {
        "normal":          scenario_normal,
        "rssi":            scenario_rssi,
        "reconnect":       scenario_reconnect,
        "stream":          scenario_stream,
        "heartbeat_stop":  scenario_heartbeat_stop,
        "all":             scenario_all,
    }

    try:
        scenarios[args.scenario]()
    except KeyboardInterrupt:
        print("\nAborted.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
