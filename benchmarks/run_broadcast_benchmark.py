#!/usr/bin/env python3
"""Run paired, equally instrumented public broadcast-to-handler samples."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import time


def system_cpu():
    if os.name != "nt":
        return None
    idle = ctypes.c_ulonglong()
    kernel = ctypes.c_ulonglong()
    user = ctypes.c_ulonglong()
    if not ctypes.windll.kernel32.GetSystemTimes(
        ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)
    ):
        return None
    return idle.value, kernel.value + user.value


def run_sample(binary, readers, size, messages, timeout):
    begin_cpu = system_cpu()
    command = [str(binary), "--readers", str(readers), "--bytes", str(size),
               "--messages", str(messages), "--warmup", str(max(64, messages // 20))]
    start = time.time()
    process = subprocess.Popen(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        start_new_session=os.name != "nt")
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           capture_output=True, check=False)
        else:
            os.killpg(process.pid, signal.SIGKILL)
        stdout, stderr = process.communicate()
    end_cpu = system_cpu()
    records = [json.loads(line) for line in stdout.splitlines() if line.startswith("{")]
    record = records[-1] if records else {"complete": False}
    if timed_out:
        record["complete"] = False
    record.update(command=command, exit_code="timeout" if timed_out else process.returncode,
                  started_unix=start, stdout=stdout if timed_out else "", stderr=stderr,
                  host=platform.node(), cpu_count=os.cpu_count())
    if begin_cpu and end_cpu and end_cpu[1] > begin_cpu[1]:
        record["host_cpu_percent"] = 100 * (
            1 - (end_cpu[0] - begin_cpu[0]) / (end_cpu[1] - begin_cpu[1]))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--readers", type=int, nargs="+", default=[1, 2, 4, 8, 16])
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    workloads = [(64, 1000000), (4096, 100000), (261632, 4096)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    samples = []
    with args.output.open("w", encoding="utf-8") as output:
        for readers in args.readers:
            for size, messages in workloads:
                for repeat in range(args.repeats):
                    order = ["baseline", "candidate"] if repeat % 2 == 0 else ["candidate", "baseline"]
                    pair = []
                    for version in order:
                        record = run_sample(getattr(args, version), readers, size, messages, args.timeout)
                        record.update(version=version, repeat=repeat, readers=readers, payload_bytes=size)
                        pair.append(record)
                    comparable = all(record.get("complete") and record.get("exit_code") == 0
                                     for record in pair)
                    for record in pair:
                        record["comparable_pair"] = comparable
                        output.write(json.dumps(record) + "\n")
                        output.flush()
                        samples.append(record)
                    print(f"readers={readers} bytes={size} pair={repeat + 1} comparable={comparable}", flush=True)
    for readers in args.readers:
        for size, _ in workloads:
            values = {}
            for version in ("baseline", "candidate"):
                selected = [sample["delivered_per_second"] for sample in samples
                            if sample["readers"] == readers and sample["payload_bytes"] == size
                            and sample["version"] == version and sample["comparable_pair"]]
                values[version] = statistics.median(selected) if selected else None
            print(json.dumps(dict(readers=readers, payload_bytes=size, medians=values)))


if __name__ == "__main__":
    main()
