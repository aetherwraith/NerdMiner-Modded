#!/usr/bin/env python3
"""
Lightweight static check to catch nested/recursive locks on non-recursive
std::mutex globals (especially g_settingsMutex).
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "src")

# Functions known to acquire g_settingsMutex internally.
LOCKING_HELPERS = {
    "activePoolAddress",
    "activePoolPort",
    "activePoolPassword",
    "backupPoolSet",
    "currentSettings",
    "fillMinerStatusJson",
    "applyLiveSettings",
}

CALL_RE = re.compile(r"\b(" + "|".join(sorted(LOCKING_HELPERS)) + r")\s*\(")
LOCK_RE = re.compile(r"std::lock_guard<\s*std::mutex\s*>\s+\w+\s*\(\s*g_settingsMutex\s*\)")


def strip_comments(text):
    text = re.sub(r"//.*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return text


def check_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()
    cleaned = strip_comments(raw)
    lines = cleaned.splitlines()
    errors = []

    depth = 0
    lock_depths = []
    for lineno, line in enumerate(lines, start=1):
        if LOCK_RE.search(line):
            lock_depths.append(depth)
        if lock_depths:
            m = CALL_RE.search(line)
            if m:
                errors.append((lineno, m.group(1), line.strip()))
        depth += line.count("{") - line.count("}")
        while lock_depths and depth < lock_depths[-1]:
            lock_depths.pop()
    return errors


def main():
    files = sorted(glob.glob(os.path.join(SRC_DIR, "**", "*.cpp"), recursive=True))
    all_errors = []
    for path in files:
        for lineno, fn, snippet in check_file(path):
            rel = os.path.relpath(path, ROOT)
            all_errors.append(f"{rel}:{lineno}: calls {fn}() while holding g_settingsMutex: {snippet}")

    if all_errors:
        print("MUTEX LOCK GUARD FAILED:")
        for err in all_errors:
            print(" ", err)
        sys.exit(1)

    print(f"OK: {len(files)} C++ source file(s) checked; no nested g_settingsMutex calls found.")


if __name__ == "__main__":
    main()
