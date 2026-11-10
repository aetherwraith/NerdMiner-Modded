#!/usr/bin/env python3
"""
NerdMiner Fleet Flash, Provisioning, LAN Discovery & Rolling OTA Utility
Supports both USB serial provisioning and zero-dependency LAN fleet management
(status summary, subnet auto-discovery, config broadcast, physical locator, and OTA).
"""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import glob
import http.client
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

ESPTOOL_PATH = os.path.expanduser("~/.local/bin/pio")
PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_DIR, ".pio", "build", "ESP32-2432S028R")
DEFAULT_BIN = os.path.join(BUILD_DIR, "firmware.bin")

DEFAULT_FLEET_IPS = [
    "192.168.4.32",   # nm1
    "192.168.4.108",  # nm2
    "192.168.4.114",  # nm3
    "192.168.4.107",  # nm4
    "192.168.4.113",  # nm5
    "192.168.4.109",  # nm6
    "192.168.4.110",  # nm7
    "192.168.4.115",  # nm8
    "192.168.4.112",  # nm9
]

BASE_WALLET = os.environ.get("NERDMINER_WALLET", "qp97c55nspk0jgd6swf5h06lrygshflsvg6seeay6m")
BASE_CONFIG = {
    "poolString": os.environ.get("NERDMINER_POOL", "bch.nexuspool.io"),
    "portNumber": int(os.environ.get("NERDMINER_PORT", "3351")),
    "poolPassword": os.environ.get("NERDMINER_POOL_PW", "x"),
    "gmtZone": "Europe/London",
    "saveStatsToNVS": True,
    "invertColors": True,
    "Brightness": 50,
    "flipDisplay": False,
    "currency": "gbp",
    "autoBrightness": True,
    "screenTimeout": 1,
    "nightDim": False,
    "nightBrightness": 40,
    "nightFade": 60,
    "latitude": 54.60,
    "longitude": -5.93,
    "ledScreenOff": True,
    "SSID": os.environ.get("NERDMINER_WIFI_SSID", ""),
    "WifiPW": os.environ.get("NERDMINER_WIFI_PW", ""),
}


def generate_config(suffix):
    cfg = dict(BASE_CONFIG)
    cfg["btcString"] = f"{BASE_WALLET}.{suffix}"
    cfg["hostname"] = suffix
    return cfg


def get_connected_ports():
    return sorted(glob.glob("/dev/ttyUSB*"))


def http_json(host, path, method="GET", payload=None, timeout=3.0, auth=None):
    url = f"http://{host}{path}"
    headers = {"Accept": "application/json"}
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if auth:
        import base64
        token = base64.b64encode(auth.encode("utf-8")).decode("ascii")
        headers["Authorization"] = f"Basic {token}"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def discover_subnet(subnet_prefix="192.168.4.", timeout=1.0):
    prefix = subnet_prefix if subnet_prefix.endswith(".") else subnet_prefix + "."
    found = []

    def probe_ip(ip):
        try:
            d = http_json(ip, "/probe", timeout=timeout)
            if isinstance(d, dict) and (d.get("nerdminer") or "host" in d or "hostname" in d or "ver" in d):
                return (ip, d)
        except Exception:
            pass
        return None

    print(f"Scanning {prefix}1..254 for NerdMiner devices...")
    with ThreadPoolExecutor(max_workers=32) as ex:
        futures = [ex.submit(probe_ip, f"{prefix}{i}") for i in range(1, 255)]
        for fut in as_completed(futures):
            res = fut.result()
            if res:
                found.append(res)
    found.sort(key=lambda item: (item[1].get("host") or item[1].get("hostname") or "", item[0]))
    print(
        f"Discovered {len(found)} device(s): "
        + ", ".join(f"{d.get('host') or d.get('hostname') or '?'} ({ip})" for ip, d in found)
    )
    return [ip for ip, _ in found]


def fmt_si(val):
    try:
        v = float(val)
    except Exception:
        return str(val)
    for unit, scale in (("T", 1e12), ("G", 1e9), ("M", 1e6), ("K", 1e3)):
        if v >= scale:
            return f"{v / scale:.2f}{unit}"
    return f"{v:.4g}"


def fmt_uptime(sec):
    sec = int(sec or 0)
    d, rem = divmod(sec, 86400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    if d:
        return f"{d}d {h:02d}h"
    if h:
        return f"{h}h {m:02d}m"
    return f"{m}m {s:02d}s"


def fleet_status(hosts, auth=None):
    rows = []
    with ThreadPoolExecutor(max_workers=min(16, max(1, len(hosts)))) as ex:
        fut_map = {ex.submit(http_json, h, "/api/status", "GET", None, 3.0, auth): h for h in hosts}
        for fut in as_completed(fut_map):
            h = fut_map[fut]
            try:
                rows.append((h, fut.result(), None))
            except Exception as exc:
                rows.append((h, None, str(exc)))

    rows.sort(key=lambda r: (r[1].get("hostname", "~") if r[1] else "~", r[0]))
    total_kh = 0.0
    total_shares = 0
    best_all = 0.0
    best_host = "-"
    online = 0

    header = f"{'HOST':<8} {'IP':<15} {'VER':<9} {'RATE':>10} {'SHARES':>8} {'SESS BEST':>10} {'ALL BEST':>10} {'PING':>7} {'RSSI':>7} {'UPTIME':>9}"
    print(header)
    print("-" * len(header))
    for h, st, err in rows:
        if not st:
            print(f"{'OFFLINE':<8} {h:<15} {'-':<9} {'-':>10} {'-':>8} {'-':>10} {'-':>10} {'-':>7} {'-':>7} {err}")
            continue
        online += 1
        name = st.get("hostname") or "-"
        ver = st.get("version") or "-"
        kh = float(st.get("hashrate_khs") or 0.0)
        sh = int(st.get("shares_accepted") or st.get("shares_32bit") or 0)
        sbest = float(st.get("session_best_difficulty") or 0.0)
        abest = float(st.get("best_difficulty") or 0.0)
        pool = st.get("pool") or {}
        ping = pool.get("ping_ms")
        ping_s = f"{ping}ms" if ping is not None and int(ping) >= 0 else "-"
        wifi = st.get("wifi") or {}
        rssi = wifi.get("rssi")
        rssi_s = f"{rssi}dBm" if rssi is not None else "-"
        up = fmt_uptime(st.get("uptime_s"))

        total_kh += kh
        total_shares += sh
        if abest > best_all:
            best_all = abest
            best_host = name

        print(
            f"{name:<8} {h:<15} {ver:<9} {kh:>7.1f}KH/s {sh:>8} "
            f"{fmt_si(sbest):>10} {fmt_si(abest):>10} {ping_s:>7} {rssi_s:>7} {up:>9}"
        )

    print("-" * len(header))
    rate_str = f"{total_kh / 1000.0:.2f} MH/s" if total_kh >= 1000 else f"{total_kh:.1f} KH/s"
    print(
        f"Online: {online}/{len(hosts)} | Fleet Hashrate: {rate_str} | "
        f"Total Accepted Shares: {total_shares} | Fleet Best: {fmt_si(best_all)} ({best_host})"
    )


def upload_ota_one(host, bin_bytes, filename="firmware.bin", auth=None, timeout=60):
    boundary = f"----NerdMinerOTA{uuid.uuid4().hex}"
    head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="update"; filename="{filename}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode("utf-8")
    tail = f"\r\n--{boundary}--\r\n".encode("utf-8")
    body = head + bin_bytes + tail

    headers = {
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Content-Length": str(len(body)),
        "Connection": "close",
    }
    if auth:
        import base64
        headers["Authorization"] = "Basic " + base64.b64encode(auth.encode("utf-8")).decode("ascii")

    # http.client sends headers + body without Expect: 100-continue, which ESP32 WebServer requires.
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        conn.request("POST", "/update", body=body, headers=headers)
        resp = conn.getresponse()
        resp_text = resp.read().decode("utf-8", errors="replace").strip()
        return resp.status, resp_text
    finally:
        conn.close()


def fleet_ota(hosts, bin_path, concurrency=3, auth=None):
    if not os.path.isfile(bin_path):
        print(f"Firmware binary not found: {bin_path}")
        return False
    with open(bin_path, "rb") as f:
        bin_bytes = f.read()
    print(f"Loaded {bin_path} ({len(bin_bytes):,} bytes). Flashing {len(hosts)} device(s) (concurrency={concurrency})...")

    ok_hosts = []

    def worker(h):
        t0 = time.time()
        status, text = upload_ota_one(h, bin_bytes, os.path.basename(bin_path), auth=auth)
        return h, status, text, time.time() - t0

    with ThreadPoolExecutor(max_workers=max(1, concurrency)) as ex:
        futs = {ex.submit(worker, h): h for h in hosts}
        for fut in as_completed(futs):
            h = futs[fut]
            try:
                _, code, text, dt = fut.result()
                if 200 <= code < 300:
                    print(f"  [OK]   {h:<15} HTTP {code} in {dt:.1f}s")
                    ok_hosts.append(h)
                else:
                    print(f"  [FAIL] {h:<15} HTTP {code}: {text[:120]}")
            except Exception as exc:
                print(f"  [ERR]  {h:<15} {exc}")

    if ok_hosts:
        print("Waiting 8s for devices to reboot and verify new firmware...")
        time.sleep(8.0)
        for h in ok_hosts:
            for _ in range(5):
                try:
                    p = http_json(h, "/probe", timeout=2.5, auth=auth)
                    hname = p.get("host") or p.get("hostname") or "?"
                    ver = p.get("ver") or p.get("version") or "?"
                    print(f"  [BOOT] {h:<15} -> {hname} running {ver}")
                    break
                except Exception:
                    time.sleep(2.0)
            else:
                print(f"  [WARN] {h:<15} did not answer /probe yet")
    return len(ok_hosts) == len(hosts)


def fleet_broadcast(hosts, patch, auth=None):
    print(f"Broadcasting config patch to {len(hosts)} device(s): {json.dumps(patch)}")
    with ThreadPoolExecutor(max_workers=min(16, max(1, len(hosts)))) as ex:
        futs = {ex.submit(http_json, h, "/api/config", "POST", patch, 4.0, auth): h for h in hosts}
        for fut in as_completed(futs):
            h = futs[fut]
            try:
                res = fut.result()
                print(f"  [OK]   {h:<15} -> {res}")
            except Exception as exc:
                print(f"  [ERR]  {h:<15} -> {exc}")


def fleet_identify(hosts, duration_ms=10000, auth=None):
    for h in hosts:
        try:
            res = http_json(h, "/api/identify", "POST", {"durationMs": duration_ms}, 3.0, auth)
            print(f"  [LOCATE] {h:<15} -> {res}")
        except Exception as exc:
            print(f"  [ERR]    {h:<15} -> {exc}")


def flash_device(port, baud=921600):
    bootloader = os.path.join(BUILD_DIR, "bootloader.bin")
    partitions = os.path.join(BUILD_DIR, "partitions.bin")
    firmware = os.path.join(BUILD_DIR, "firmware.bin")

    cmd = [
        ESPTOOL_PATH, "pkg", "exec", "-p", "tool-esptoolpy", "--",
        "esptool.py", "--port", port, "--baud", str(baud),
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", "-z",
        "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
        "0x1000", bootloader,
        "0x8000", partitions,
        "0x10000", firmware,
    ]
    print(f"[{port}] Flashing bootloader, partitions (dual-OTA), and firmware...")
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        print(f"[{port}] Flashing failed! Error:\n{res.stderr}\n{res.stdout}")
        return False
    print(f"[{port}] Flashing SUCCESS!")
    return True


def provision_device(port, suffix, retries=5):
    import serial
    cfg = generate_config(suffix)
    payload = json.dumps(cfg)
    cmd = f"config json {payload}\n"

    print(f"[{port}] Waiting for device boot and provisioning as '{suffix}'...")
    time.sleep(3.5)
    for attempt in range(1, retries + 1):
        try:
            s = serial.Serial(port, 115200, timeout=3)
            time.sleep(1.0)
            if s.in_waiting:
                s.read(s.in_waiting)

            s.write(b"\n")
            time.sleep(0.5)
            s.read(s.in_waiting or 100)

            s.write(cmd.encode("utf-8"))
            time.sleep(2.0)
            out = s.read(s.in_waiting or 2000).decode("utf-8", errors="replace")
            print(f"[{port}] Config reply:\n{out.strip()}")

            if "success" in out or "Restarting" in out or "applied" in out:
                time.sleep(4.0)
                s.write(b"\nprobe\n")
                time.sleep(1.0)
                probe_out = s.read(s.in_waiting or 500).decode("utf-8", errors="replace")
                print(f"[{port}] Probe check: {probe_out.strip()}")
                s.close()
                return True
            s.close()
        except Exception as e:
            print(f"[{port}] Attempt {attempt} failed ({e}), retrying...")
            time.sleep(2)
    return False


def main():
    parser = argparse.ArgumentParser(description="Flash, provision, monitor, and OTA-update NerdMiner fleet")
    parser.add_argument("--port", help="Specific USB serial port to flash")
    parser.add_argument("--id", help="Worker/Hostname suffix for USB provisioning (e.g. nm1)")
    parser.add_argument("--provision-only", action="store_true", help="Only USB-configure without reflashing")

    # LAN fleet modes
    parser.add_argument("--ips", help="Comma-separated list of miner IPs/hostnames (default: nm1-nm9 fleet)")
    parser.add_argument("--discover", nargs="?", const="192.168.4.", help="Scan /24 subnet prefix (e.g. 192.168.4.)")
    parser.add_argument("--status", action="store_true", help="Show live LAN fleet status table")
    parser.add_argument("--ota", nargs="?", const=DEFAULT_BIN, help="Push firmware .bin over LAN OTA to target fleet")
    parser.add_argument("--concurrency", type=int, default=3, help="Parallel LAN OTA uploads (default: 3)")
    parser.add_argument("--broadcast-json", help="JSON object string to POST to /api/config on all target miners")
    parser.add_argument("--identify", action="store_true", help="Trigger 10s LED + screen locator on target miners")
    parser.add_argument("--auth", help="Optional user:password Basic Auth when custom WebPassword is set")
    args = parser.parse_args()

    if args.discover or args.status or args.ota or args.broadcast_json or args.identify:
        if args.discover:
            hosts = discover_subnet(args.discover)
        elif args.ips:
            hosts = [x.strip() for x in args.ips.split(",") if x.strip()]
        else:
            hosts = list(DEFAULT_FLEET_IPS)

        if not hosts:
            print("No target hosts found.")
            sys.exit(1)

        if args.identify:
            fleet_identify(hosts, auth=args.auth)
        if args.broadcast_json:
            fleet_broadcast(hosts, json.loads(args.broadcast_json), auth=args.auth)
        if args.ota:
            if not fleet_ota(hosts, args.ota, concurrency=args.concurrency, auth=args.auth):
                sys.exit(1)
        if args.status or (args.discover and not (args.ota or args.broadcast_json or args.identify)):
            fleet_status(hosts, auth=args.auth)
        return

    if args.port and args.id:
        if not args.provision_only:
            if not flash_device(args.port):
                sys.exit(1)
            time.sleep(3)
        if not provision_device(args.port, args.id):
            sys.exit(1)
        print(f"Successfully processed {args.port} as {args.id}!")
        return

    ports = get_connected_ports()
    print(f"Found {len(ports)} USB serial ports: {ports}")
    if not ports:
        print("No /dev/ttyUSB ports found (tip: use --status, --discover, or --ota for LAN fleet operations).")
        sys.exit(1)

    for i, port in enumerate(ports, start=1):
        suffix = f"nm{i}"
        print("\n==========================================")
        print(f"Processing {port} -> {suffix}")
        print("==========================================")
        if not args.provision_only:
            if not flash_device(port):
                print(f"Skipping {port} due to flash failure.")
                continue
            time.sleep(3)
        provision_device(port, suffix)


if __name__ == "__main__":
    main()
