#!/usr/bin/env python3
"""
Lightweight static check to catch nested/recursive locks on non-recursive
std::mutex globals (g_settingsMutex and s_otaMutex).
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "src")

# (mutex_name, set of functions known to acquire that mutex internally)
MUTEX_RULES = [
    (
        "g_settingsMutex",
        {
            "activePoolAddress",
            "activePoolPort",
            "activePoolPassword",
            "backupPoolSet",
            "currentSettings",
            "fillMinerStatusJson",
            "applyLiveSettings",
        },
    ),
    (
        "s_otaMutex",
        {
            "getOtaStatus",
            "setOtaError",
            "startUrlOta",
            "checkGitHubUpdate",
            "startGitHubOta",
        },
    ),
]

COMPILED_RULES = [
    (
        mutex_name,
        re.compile(rf"std::lock_guard<\s*std::mutex\s*>\s+\w+\s*\(\s*{re.escape(mutex_name)}\s*\)"),
        re.compile(r"\b(" + "|".join(sorted(helpers)) + r")\s*\("),
    )
    for mutex_name, helpers in MUTEX_RULES
]


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
    lock_depths = {mutex_name: [] for mutex_name, _, _ in COMPILED_RULES}
    for lineno, line in enumerate(lines, start=1):
        for mutex_name, lock_re, call_re in COMPILED_RULES:
            if lock_re.search(line):
                lock_depths[mutex_name].append(depth)
            if lock_depths[mutex_name]:
                m = call_re.search(line)
                if m:
                    errors.append((lineno, mutex_name, m.group(1), line.strip()))
        depth += line.count("{") - line.count("}")
        for depths in lock_depths.values():
            while depths and depth < depths[-1]:
                depths.pop()
    return errors


def main():
    files = sorted(glob.glob(os.path.join(SRC_DIR, "**", "*.cpp"), recursive=True))
    all_errors = []
    for path in files:
        for lineno, mutex_name, fn, snippet in check_file(path):
            rel = os.path.relpath(path, ROOT)
            all_errors.append(f"{rel}:{lineno}: calls {fn}() while holding {mutex_name}: {snippet}")

    if all_errors:
        print("MUTEX LOCK GUARD FAILED:")
        for err in all_errors:
            print(" ", err)
        sys.exit(1)

    checked_mutexes = " or ".join(m for m, _ in MUTEX_RULES)
    print(f"OK: {len(files)} C++ source file(s) checked; no nested {checked_mutexes} calls found.")


if __name__ == "__main__":
    main()
