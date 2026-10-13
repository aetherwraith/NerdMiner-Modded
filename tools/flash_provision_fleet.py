#!/usr/bin/env python3
"""
NerdMiner Fleet Flash & Provisioning Utility
Flashes NerdMiner firmware and configures unique worker names and hostnames.
"""

import sys
import os
import time
import json
import glob
import subprocess
import argparse

ESPTOOL_PATH = os.path.expanduser("~/.local/bin/pio")
PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_DIR, ".pio", "build", "ESP32-2432S028R")

BASE_WALLET = os.environ.get("NERDMINER_WALLET", "qp97c55nspk0jgd6swf5h06lrygshflsvg6seeay6m")
BASE_CONFIG = {
    "poolString": os.environ.get("NERDMINER_POOL", "bch.nexuspool.io"),
    "portNumber": int(os.environ.get("NERDMINER_PORT", "3351")),
    "poolPassword": os.environ.get("NERDMINER_POOL_PW", "x"),
    "gmtZone": "Europe/London",
    "saveStatsToNVS": False,
    "invertColors": True,
    "Brightness": 50,
    "flipDisplay": False,
    "currency": "gbp",
    "autoBrightness": True,
    "screenOffTimeout": 120,
    "screenTimeout": 0,
    "nightDim": False,
    "nightBrightness": 40,
    "nightFade": 60,
    "latitude": 0,
    "longitude": 0,
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
        "0x10000", firmware
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
            # Drain boot messages
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
    parser = argparse.ArgumentParser(description="Flash and provision CYD NerdMiner fleet")
    parser.add_argument("--port", help="Specific serial port to flash")
    parser.add_argument("--id", help="Worker/Hostname suffix (e.g. nm1, nm2)")
    parser.add_argument("--provision-only", action="store_true", help="Only configure without reflashing")
    args = parser.parse_args()

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
        print("No /dev/ttyUSB ports found.")
        sys.exit(1)

    for i, port in enumerate(ports, start=1):
        suffix = f"nm{i}"
        print(f"\n==========================================")
        print(f"Processing {port} -> {suffix}")
        print(f"==========================================")
        if not args.provision_only:
            if not flash_device(port):
                print(f"Skipping {port} due to flash failure.")
                continue
            time.sleep(3)
        provision_device(port, suffix)

if __name__ == "__main__":
    main()
