#!/usr/bin/env python3
"""
analyze_ds_session.py — pull a deep-sleep session off the relay and
emit the soak-test metrics we care about.

Usage:
    ./analyze_ds_session.py session_NNNNN
    ./analyze_ds_session.py session_NNNNN --interval 600

The --interval flag (in seconds) lets the analyzer compute expected
cadence drift; defaults to whatever session.json says (intervalSec).

Outputs:
  - captureCount + validThermCount
  - per-capture visOk/thermOk grid
  - missed/failed counts
  - avg / max / min wakeMs
  - drift range (actual vs intended epoch deltas)
  - sleep span between captures
  - raw16 file count + size check
  - session.json complete flag
  - any anomalies (centerRaw outliers, missing fields, etc.)
"""

from __future__ import annotations

import argparse
import os
import sys
import urllib.request
import urllib.error
import json
import statistics

RELAY    = "https://project-grasshopper-production.up.railway.app"
DEVICE   = "grasshopper-dev-001"
TOKEN    = os.environ.get(
    "GH_RELAY_TOKEN",
    "de6efd20a0e802151317ef3f68f983a8c74aadb83924f2bb42f6c091e6a5a7ef",
)

EXPECTED_RAW16_BYTES = 38400  # 160 × 120 × 2


def get(path: str, *, head: bool = False, max_tries: int = 30) -> tuple[int, dict, bytes]:
    """GET (or HEAD) a relay endpoint with the Bearer header. Retries
    on 5xx and 409 ("device offline") because the device WS thrashes
    every few seconds — most calls land on the second or third try.
    Returns (status, headers, body). Body is empty on HEAD requests."""
    import time
    url = f"{RELAY}{path}"
    last_status, last_headers, last_body = 0, {}, b""
    for i in range(max_tries):
        req = urllib.request.Request(
            url,
            headers={"Authorization": f"Bearer {TOKEN}"},
            method="HEAD" if head else "GET",
        )
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                body = b"" if head else resp.read()
                return resp.status, dict(resp.headers), body
        except urllib.error.HTTPError as e:
            last_status = e.code
            last_headers = dict(e.headers or {})
            last_body = e.read() if not head else b""
            # Retry transient: device offline / bad gateway / mid-cmd
            if e.code in (409, 502, 503, 504):
                time.sleep(2)
                continue
            return e.code, last_headers, last_body
        except urllib.error.URLError:
            time.sleep(2)
            continue
    return last_status, last_headers, last_body


def fmt_drift(seconds: float) -> str:
    sign = "+" if seconds >= 0 else "-"
    return f"{sign}{abs(seconds):6.1f}s"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("session_id", help='e.g. "session_42017"')
    ap.add_argument("--interval", type=int, default=None,
                    help="expected interval in seconds (overrides session.json)")
    ap.add_argument("--device", default=DEVICE)
    args = ap.parse_args()

    sid = args.session_id

    # ---- Fetch the per-session bundle ----
    status, _, body = get(f"/api/devices/{args.device}/sessions/{sid}")
    if status != 200:
        print(f"FAIL: relay returned {status} for /sessions/{sid}", file=sys.stderr)
        print(body.decode("utf-8", "replace")[:200], file=sys.stderr)
        return 1
    data = json.loads(body)
    meta = data.get("meta") or {}
    captures = data.get("captures") or []
    interval = args.interval or meta.get("intervalSec") or 0

    # ---- Header ----
    print(f"=== {sid} ===")
    print(f"  mode             : {meta.get('mode')}")
    print(f"  complete         : {meta.get('complete')}")
    print(f"  intervalSec      : {interval}")
    print(f"  captureVis       : {meta.get('captureVis')}")
    print(f"  captureTherm     : {meta.get('captureTherm')}")
    print(f"  meta.captureCount: {meta.get('captureCount')}")
    print(f"  journal length   : {len(captures)}")

    cstats = meta.get("tempStats") or {}
    if cstats:
        print(f"  meta.validThermCount: {cstats.get('validThermCount')}")
        print(f"  meta.minTempF / maxTempF / avgCenterTempF: "
              f"{cstats.get('minTempF')} / {cstats.get('maxTempF')} / {cstats.get('avgCenterTempF')}")
    print()

    # ---- Per-capture analysis ----
    if not captures:
        print("FAIL: journal is empty", file=sys.stderr)
        return 1

    vis_failed = sum(1 for c in captures if not c.get("visOk"))
    therm_failed = sum(1 for c in captures if not c.get("thermOk"))
    valid_therm = sum(1 for c in captures if c.get("thermOk") and c.get("tlinearResolution"))

    wake_ms = [c.get("wakeMs") for c in captures if c.get("wakeMs") is not None]
    visible_ms = [c.get("visibleMs") for c in captures if c.get("visibleMs") is not None]
    thermal_ms = [c.get("thermalMs") for c in captures if c.get("thermalMs") is not None]

    intended_epochs = [c.get("intendedEpoch") for c in captures if c.get("intendedEpoch")]
    actual_epochs   = [c.get("timestamp") for c in captures if c.get("timestamp")]
    drifts = []
    for c in captures:
        ie = c.get("intendedEpoch") or 0
        ae = c.get("timestamp") or 0
        if ie > 0 and ae > 0:
            drifts.append(ae - ie)

    sleep_spans = []
    for i in range(1, len(actual_epochs)):
        sleep_spans.append(actual_epochs[i] - actual_epochs[i-1])

    print("--- per-capture grid ---")
    print("seq | visOk | thermOk | wakeMs | visBytes | thermBytes |   actual    |  intended   | drift  | tq")
    for c in captures:
        print(f"{c.get('seq'):>3} |  {'T' if c.get('visOk') else 'F'}    |   {'T' if c.get('thermOk') else 'F'}     "
              f"| {c.get('wakeMs') or 0:>6} | {c.get('visBytes') or 0:>8} | {c.get('thermBytes') or 0:>10} "
              f"| {c.get('timestamp') or 0} | {c.get('intendedEpoch') or 0} "
              f"| {fmt_drift((c.get('timestamp') or 0) - (c.get('intendedEpoch') or 0))} "
              f"| {c.get('timeQuality') if c.get('timeQuality') is not None else '?'}")
    print()

    print("--- summary metrics ---")
    print(f"  captures landed       : {len(captures)} / expected {meta.get('captureCount')}")
    print(f"  validThermCount       : {valid_therm}")
    print(f"  visible failures      : {vis_failed}")
    print(f"  thermal failures      : {therm_failed}")
    if wake_ms:
        print(f"  wakeMs   avg/max/min  : {statistics.mean(wake_ms):.0f} / {max(wake_ms)} / {min(wake_ms)}")
    if visible_ms:
        print(f"  visibleMs avg         : {statistics.mean(visible_ms):.0f}")
    if thermal_ms:
        print(f"  thermalMs avg         : {statistics.mean(thermal_ms):.0f}")
    if drifts:
        print(f"  drift range (s)       : min={min(drifts)} max={max(drifts)} avg={statistics.mean(drifts):.1f}")
    if sleep_spans:
        avg_span = statistics.mean(sleep_spans)
        ideal = interval
        print(f"  sleep span avg / ideal: {avg_span:.1f}s / {ideal}s "
              f"({(avg_span - ideal):+.1f}s cadence error)")
        print(f"  sleep span min / max  : {min(sleep_spans)}s / {max(sleep_spans)}s")

    # ---- raw16 file integrity ----
    # The relay serves session files via session.read_file cmd (chunked),
    # so HEAD doesn't give a useful Content-Length. GET the body and
    # measure. ~38KB × N captures is negligible bandwidth.
    print()
    print("--- raw16 file integrity ---")
    raw_ok = 0
    raw_missing = 0
    raw_wrong_size = 0
    for c in captures:
        if not c.get("thermOk"):
            continue
        fname = f"{c['seq']:06d}_therm.raw16"
        status, _, body = get(
            f"/api/devices/{args.device}/sessions/{sid}/file/{fname}",
        )
        if status != 200:
            raw_missing += 1
            print(f"  seq {c['seq']:>3}: MISSING (status {status})")
            continue
        size = len(body)
        if size == EXPECTED_RAW16_BYTES:
            raw_ok += 1
        else:
            raw_wrong_size += 1
            print(f"  seq {c['seq']:>3}: WRONG SIZE {size} (expected {EXPECTED_RAW16_BYTES})")
    print(f"  raw16 OK={raw_ok}, missing={raw_missing}, wrong-size={raw_wrong_size}")

    # ---- Pass/fail verdict ----
    print()
    print("--- verdict ---")
    fail_conditions = []
    if not meta.get("complete"):
        fail_conditions.append("session.json complete != true")
    if len(captures) != meta.get("captureCount"):
        fail_conditions.append(
            f"journal length {len(captures)} != meta.captureCount {meta.get('captureCount')}"
        )
    if vis_failed > 0 and meta.get("captureVis"):
        fail_conditions.append(f"{vis_failed} visible failures")
    if therm_failed > 0 and meta.get("captureTherm"):
        fail_conditions.append(f"{therm_failed} thermal failures")
    if raw_missing > 0:
        fail_conditions.append(f"{raw_missing} raw16 missing")
    if raw_wrong_size > 0:
        fail_conditions.append(f"{raw_wrong_size} raw16 wrong size")
    if drifts and max(drifts) - min(drifts) > 60:
        fail_conditions.append(
            f"drift range {min(drifts)}..{max(drifts)} exceeds 60s tolerance"
        )

    if not fail_conditions:
        print("  PASS — soak baseline clean")
        return 0
    print("  FAIL:")
    for f in fail_conditions:
        print(f"    - {f}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
