#!/usr/bin/env python3
"""Level 2 of the benchmark CI: a paired comparison, on one hosted runner, of the base library
(the previous master) with the head library (the pushed commit), both built with head's
benchmarks. It reports and never gates (docs/benchmark_plan.md, section 15).

  run      measures the cells of workloads_ci.json with six runner calls per cell
  report   classifies the job's outcome and writes the platform's verdicts and summary
  collect  combines the platforms' summaries of one workflow run into the run's files
  archive  adds those files to the benchmark-history branch

See benchmarks/README.md, "Continuous integration".
"""

import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import traceback

import run_benchmarks as runner

CI_WORKLOADS_SCHEMA = "sintra-bench-ci-workloads/1"
JOB_FORMAT = "sintra-bench-ci-job/1"
SUMMARY_FORMAT = "sintra-bench-ci-summary/1"
HISTORY_FORMAT = "sintra-bench-ci-history/1"
PLATFORMS = ["linux", "windows", "macos", "freebsd"]
PLATFORM_NAMES = {"linux": "Linux", "windows": "Windows", "macos": "macOS", "freebsd": "FreeBSD"}
RUNNER = Path(__file__).with_name("run_benchmarks.py")

# The six runner calls of a cell, in order: kind, pairs, and the directories of the two
# executable copies that the call runs. Every call gets fresh copies, and all directory names
# have one length. An A/A call runs two copies of arm A; the runner then writes `aa: false`,
# since the call passes --b, so a record's kind is read from its --note. 5 A/A and 5 A/B
# pairs, at mean positions 5.4 and 5.6 of 10; the A/B pairs run A first three times.
SCHEDULE = [
    ("aa", 1, "a1", "a2"),
    ("ab", 2, "a7", "b1"),
    ("aa", 2, "a3", "a4"),
    ("ab", 2, "a8", "b2"),
    ("aa", 2, "a5", "a6"),
    ("ab", 1, "a9", "b3"),
]
PAIRS = {kind: sum(pairs for each, pairs, _, _ in SCHEDULE if each == kind)
         for kind in ("aa", "ab")}
SAMPLES_PER_CELL = 2 * sum(PAIRS.values())
PRICE_FACTOR = 1.5          # a cell is priced at its samples times their expected time, plus 50 %
STOP_GRACE_S = 30           # after the hard stop's interrupt, for the runner to end its swarm
RULE = "same-direction-beyond-aa-spread/1"
WRAP_NS = 2 ** 62           # a cross-process latency at or above this is an unsigned wrap
CLOCK_TICK = {"windows": "0.1 µs", "macos": "1/24 µs"}
CROSS_PROCESS = ("slowest_from_send",)
# A build step's outcome other than success and failure, as the report states it. `missing`:
# the FreeBSD VM script wrote no outcome file.
ARM_OUTCOMES = {"skipped": "did not run (an earlier step failed)",
                "cancelled": "was cancelled",
                "missing": "left no build outcome (the VM script did not get to it)"}

HISTORY_BRANCH = "benchmark-history"
HISTORY_BUDGET_BYTES = 8_000_000
HISTORY_WARNING_SHARE = 0.8
PUSH_ATTEMPTS = 5
BOT_NAME = "github-actions[bot]"
BOT_EMAIL = "41898282+github-actions[bot]@users.noreply.github.com"

WORDING = (
    "Report only: no number ever fails CI. GitHub-hosted runners are shared, noisy machines. "
    "`regressed` or `improved` means that all five A/B pairs moved the same way, each by more "
    "than any of this job's five A/A pairs: a lead to look into, not proof.")
NULL_WORDING = "Null test: the library did not change, so any verdict here is a false call."


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, data):
    """Writes atomically: a temporary file, then a rename."""
    temporary = Path(path).with_name(Path(path).name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps(data, indent=1) + "\n")
    os.replace(temporary, path)


def write_text(path, text):
    with Path(path).open("w", encoding="utf-8", newline="\n") as output:
        output.write(text)


def executable_name(workload):
    return runner.EXECUTABLES[workload] + (".exe" if os.name == "nt" else "")


def workload_of(cell_id):
    return {prefix: workload for workload, prefix in runner.CELL_PREFIXES.items()}[
        cell_id.split(".")[0]]


def primary_metrics(cell_id):
    """(label, higher is better) of the cell's primary metrics (plan section 15.6). All of
    them are measured within one process."""
    workload, mode = workload_of(cell_id), cell_id.split(".")[1]
    if workload == "broadcast":
        return [("send_call p50 us", False), ("publication msg/s", True),
                ("CPU us/msg total", False)]
    if workload == "rpc" and mode == "closed_loop":
        return [("calls/s", True), ("CPU us/call total", False)]
    if workload == "rpc":
        return [("issue_to_observation p50 us", False), ("CPU us/call total", False)]
    return [("time_to_ready s", False), ("time_to_shutdown s", False),
            ("formation CPU s total", False)]


def verdict_exclusion(label, platform):
    """Why a primary metric gets no verdict on the platform whatever its values; None when it
    can get one."""
    if label.startswith("send_call") and platform in CLOCK_TICK:
        return f"the clock ticks in steps of {CLOCK_TICK[platform]} here"
    if "CPU" in label and platform == "windows":
        return "process CPU often does not advance on Windows"
    return None


# --- run ---------------------------------------------------------------------------

def resolve_workloads(ci, platform):
    """The runner workloads file of one platform, and the CI cells in priority order.

    Raises ValueError for a file that the driver must not run; the runner's own validation of
    the resolved file exits with its message.
    """
    problems = []
    if ci.get("schema") != CI_WORKLOADS_SCHEMA:
        problems.append(f"schema must be {CI_WORKLOADS_SCHEMA}")

    def check_per_platform(value, where, check):
        if (not isinstance(value, dict) or sorted(value) != sorted(PLATFORMS)
                or not all(check(each) for each in value.values())):
            problems.append(f"{where} needs one valid value for each of {', '.join(PLATFORMS)}")

    check_per_platform(ci.get("target_interval_s"), "target_interval_s",
                       runner.is_positive_number)
    entries, leaves = [], []
    for index, cell in enumerate(ci["cells"]):
        where = f"cells[{index}]"
        check_per_platform(cell.get("sample_s"), f"{where}.sample_s", runner.is_positive_number)
        entry = dict(cell["entry"])
        counts = cell.get("counts", {})
        if entry["workload"] == "swarm":
            leaves.append(entry["leaves"])
            if counts:
                problems.append(f"{where}: a swarm cell has no count")
            continue
        if not set(counts) <= set(PLATFORMS) or not all(map(runner.is_count, counts.values())):
            problems.append(f"{where}.counts must map platforms to positive integers")
        key = "calls" if entry["workload"] == "rpc" else "messages"
        entry[key] = counts.get(platform, entry[key])
        entries.append(entry)
    if problems:
        raise ValueError("invalid CI workloads file:\n  " + "\n  ".join(problems))
    workloads = {
        "schema": "sintra-bench-workloads/1",
        "target_interval_s": ci["target_interval_s"][platform],
        "default_receivers": [],
        # The runner requires the open-loop settings; no CI cell is an open-loop cell.
        "open_loop": {"duration_s": 1, "rate_fractions": [1]},
        "swarm": {"leaves": leaves, "oversubscribed_least": 80, "oversubscribed_per_cpu": 2},
        "cells": entries,
    }
    runner.validate_workloads(workloads)
    runner.validate_swarm(workloads)
    defined = {cell["id"] for cell in
               runner.expand_cells(workloads) + runner.expand_swarm_cells(workloads)}
    ids = [cell["id"] for cell in ci["cells"]]
    problems += [f"{cell_id} is not the ID of its entry" for cell_id in ids
                 if cell_id not in defined]
    if len(set(ids)) != len(ids):
        problems.append("a cell ID repeats")
    if problems:
        raise ValueError("invalid CI workloads file:\n  " + "\n  ".join(problems))
    return workloads, ci["cells"]


def protocol_version(workloads, cells, platform):
    """What a platform's verdicts rest on: its cells and counts, the schedule with its A/A
    arms, the primary metrics with the platform's exclusions, and the verdict rule. The
    history keeps the null-test accounting per version."""
    primary = {cell["id"]: [(label, higher_is_better, verdict_exclusion(label, platform))
                            for label, higher_is_better in primary_metrics(cell["id"])]
               for cell in cells}
    text = json.dumps(
        {"ids": [cell["id"] for cell in cells], "cells": workloads["cells"],
         "leaves": workloads["swarm"]["leaves"], "schedule": SCHEDULE, "primary": primary,
         "rule": RULE},
        sort_keys=True)
    return hashlib.sha256(text.encode()).hexdigest()[:12]


def identical_executables(arms, platform):
    """Per workload, whether the two arms' executables are byte-identical: then that workload's
    cells compare identical binaries, a null test. MSVC executables differ in their link time
    stamp and debug identifiers even then, so the Windows job does not look."""
    if platform == "windows":
        return None

    def digest(arm, workload):
        path = arms / f"arm-{arm}" / "benchmarks" / executable_name(workload)
        return hashlib.sha256(path.read_bytes()).hexdigest()

    return {workload: digest("a", workload) == digest("b", workload)
            for workload in runner.EXECUTABLES}


def cpu_model():
    """The CPU model, an operational fact for the report header, not a measurement."""
    if sys.platform.startswith("linux"):
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
        return None
    if os.name == "nt":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    name = "machdep.cpu.brand_string" if sys.platform == "darwin" else "hw.model"
    return subprocess.run(["sysctl", "-n", name], capture_output=True, text=True,
                          check=True).stdout.strip()


def fresh_copy(root, arms, workload, name):
    """A new copy of one arm's executable for one runner call, in a new directory: every cell
    has a copy root of its own, so nothing is removed or reused."""
    directory = root / name / "benchmarks"
    directory.mkdir(parents=True)
    shutil.copy2(arms / f"arm-{name[0]}" / "benchmarks" / executable_name(workload), directory)
    return directory.parent


def echo(text):
    """Writes to this process's output. An output that is closed or broken, as when the step
    is being cancelled, is not an error: from then on the output goes to the null device, and
    the runner log keeps everything."""
    if sys.stdout is None:
        return
    try:
        sys.stdout.write(text)
        sys.stdout.flush()
    except (OSError, ValueError):
        sys.stdout = open(os.devnull, "w", encoding="utf-8")


def interrupt(process):
    """Asks the runner to stop as it expects it: SIGINT on POSIX, Ctrl+Break to the runner's
    own process group on Windows. The runner then ends its sample's swarm and exits."""
    process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)


def run_logged(command, log, hard_stop):
    """Runs a runner call, copying its output to this process's output and to the log. At the
    hard stop it interrupts the runner, and kills it if it has not ended STOP_GRACE_S later.

    Returns the exit code, the output lines and, when the hard stop came, what it did.
    """
    lines = []
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)

    def pump():
        for line in process.stdout:
            echo(line)
            log.write(line)
            lines.append(line.rstrip("\n"))

    reader = threading.Thread(target=pump)
    reader.start()
    stopped = None
    try:
        process.wait(timeout=max(0.0, hard_stop - time.time()))
    except subprocess.TimeoutExpired:
        try:
            interrupt(process)
            process.wait(timeout=STOP_GRACE_S)
            stopped = "the hard stop interrupted the runner, which ended its swarm and exited"
        except (OSError, subprocess.TimeoutExpired) as error:
            process.kill()
            process.wait()
            stopped = (f"the hard stop interrupted the runner, which had not exited after "
                       f"{STOP_GRACE_S} s ({error!r}), so it was killed")
    reader.join()
    log.flush()
    return process.returncode, lines, stopped


def stop_reason(lines):
    """Why a runner call that exited non-zero ended the platform, from all of its output."""
    text = "\n".join(lines)
    if "cleanup incomplete" in text:
        return "cleanup_incomplete"
    if "the toolchain or the configuration of the two arms differs" in text:
        return "configuration_differs"
    return "runner_failed"


def run_call(args, context, cell, root, number, kind, pairs, copies, workloads_path, log):
    """One runner call of a cell, appending to the cell's output file. Returns its entry for
    job.json, with the reason when it ends the platform."""
    records_path = args.output / "records" / f"{cell['id']}.jsonl"
    a, b = (fresh_copy(root, args.arms, workload_of(cell["id"]), name) for name in copies)
    note = f"ci-compare {args.platform} {kind} call {number}, benchmarks {context['head']}"
    command = [
        sys.executable, "-u", str(RUNNER),
        "--a", str(a), "--a-commit", context["base"],
        "--b", str(b), "--b-commit", context["base"] if kind == "aa" else context["head"],
        "--output", str(records_path), "--workloads", str(workloads_path),
        "--pairs", str(pairs), "--select", cell["id"], "--note", note]
    log.write(f"\n== {cell['id']}, call {number} ({kind}, {pairs} pairs, "
              f"{copies[0]}/{copies[1]})\n")
    before = len(runner.load_records(records_path))
    started = time.time()
    exit_code, lines, stopped = run_logged(command, log, args.hard_stop)
    records = runner.load_records(records_path)[before:]
    ended = stopped is not None or exit_code != 0
    return {
        "number": number, "kind": kind, "pairs": pairs, "copies": list(copies), "note": note,
        "exit_code": exit_code, "seconds": round(time.time() - started, 2),
        "records": len(records),
        "invalid": sum(1 for record in records if not record["outcome"]["valid"]),
        "stop_reason": ("deadline" if stopped else stop_reason(lines)) if ended else None,
        "hard_stop": stopped,
        "output_tail": lines[-20:] if ended else None,
    }


def stop_platform(job, index, reason, message):
    """Ends the platform in cell `index`: that cell is stopped, every later one not run."""
    job["stop"] = {"reason": reason, "message": message}
    job["cells"][index]["state"] = (
        "stopped: deadline" if reason == "deadline" else "stopped: runner failed")
    for later in job["cells"][index + 1:]:
        later["state"] = ("not_run: deadline" if reason == "deadline"
                          else "not_run: platform stopped")


def run_cells(args, job, cells, workloads_path):
    """Runs the cells in priority order. A cell starts only if its six calls, priced together,
    end before the deadline; otherwise it and every later cell are left out, so that a
    shortfall always drops the same cells. The hard stop ends a call that overruns: the driver
    then records the stop and returns, so that its step ends before the step's timeout."""
    (args.output / "records").mkdir(exist_ok=True)
    with (args.output / "runner.log").open("a", encoding="utf-8", newline="\n") as log:
        for index, (cell, entry) in enumerate(zip(cells, job["cells"])):
            price = SAMPLES_PER_CELL * cell["sample_s"][args.platform] * PRICE_FACTOR
            entry["price_s"] = round(price, 1)
            if time.time() + price > args.deadline:
                for later in job["cells"][index:]:
                    later["state"] = "not_run: budget"
                return
            entry["state"] = "running"
            write_json(args.output / "job.json", job)
            root = args.arms / "copies" / f"{index + 1:02d}"
            for number, (kind, pairs, *copies) in enumerate(SCHEDULE, 1):
                if time.time() >= args.hard_stop:
                    stop_platform(job, index, "deadline",
                                  f"the hard stop came before call {number}")
                    return
                call = run_call(args, job["context"], cell, root, number, kind, pairs, copies,
                                workloads_path, log)
                entry["calls"].append(call)
                write_json(args.output / "job.json", job)
                if call["stop_reason"]:
                    stop_platform(job, index, call["stop_reason"],
                                  call["hard_stop"] or "\n".join(call["output_tail"]))
                    return
                if call["invalid"]:
                    # A verdict needs every pair, so the cell ends here.
                    entry["state"] = "stopped: invalid sample"
                    break
            else:
                entry["state"] = "complete"
            write_json(args.output / "job.json", job)


def command_run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    job = {
        "format": JOB_FORMAT, "platform": args.platform, "context": load_json(args.context),
        "deadline": args.deadline, "hard_stop": args.hard_stop, "started": time.time(),
        "cpu_model": cpu_model(), "state": "running", "error": None, "traceback": None,
        "protocol": None, "identical_executables": None, "cells": [], "stop": None,
    }
    # Written before the first call and atomically after each: the report reads what a
    # driver that its step timeout ended had finished.
    write_json(args.output / "job.json", job)
    try:
        workloads, cells = resolve_workloads(load_json(args.workloads), args.platform)
        job["protocol"] = protocol_version(workloads, cells, args.platform)
        job["identical_executables"] = identical_executables(args.arms, args.platform)
        job["cells"] = [{"id": cell["id"], "state": "pending", "calls": []} for cell in cells]
        workloads_path = args.output / "workloads.json"
        runner.write_workloads(workloads, workloads_path)
        write_json(args.output / "job.json", job)
        run_cells(args, job, cells, workloads_path)
        job["state"] = "finished"
    except (Exception, SystemExit) as error:
        job["state"] = "error"
        job["error"] = "".join(traceback.format_exception_only(error)).strip()
        job["traceback"] = traceback.format_exc()
        raise
    finally:
        # An interrupt, such as the step timeout's, leaves the state "running".
        write_json(args.output / "job.json", job)


# --- report ------------------------------------------------------------------------

def metric_table(pairs):
    """runner.metric_values, plus the closed-loop total CPU per call, which the runner's
    summary leaves out and which is a primary metric here."""
    values = runner.metric_values(pairs)
    if pairs and pairs[0][0]["cell"]["mode"] == "closed_loop":
        values["CPU us/call total"] = (True, [
            tuple(runner.cpu_value(sample, lambda cpu: cpu["total"]["us_per_message"])
                  for sample in pair) for pair in pairs])
    return values


def evaluate(label, always_difference, aa, ab):
    """The comparison of one metric: its representation, the median, minimum and maximum of
    the A/B pairs' comparisons, and the A/A spread, the largest deviation of an A/A pair."""
    as_difference = runner.compared_as_difference(always_difference, aa + ab)
    present, compared, _ = runner.compare(ab, as_difference)
    _, aa_compared, _ = runner.compare(aa, as_difference)
    metric = {"label": label, "representation": "difference" if as_difference else "ratio",
              "aa": [list(pair) for pair in aa], "ab": [list(pair) for pair in ab],
              "comparison": None, "spread": None}
    if compared:
        metric["comparison"] = {
            "n": len(compared), "median": statistics.median(compared),
            "min": min(compared), "max": max(compared),
            "median_a": statistics.median(a for a, _ in present),
            "median_b": statistics.median(b for _, b in present)}
    if aa_compared:
        metric["spread"] = max(deviation(value, as_difference) for value in aa_compared)
    return metric


def deviation(comparison, as_difference):
    """|B-A| for a difference, |ln(B/A)| for a ratio."""
    return abs(comparison) if as_difference else abs(math.log(comparison))


def give_verdict(metric, higher_is_better, exclusion):
    """regressed or improved: all 10 pairs have a value, the 5 A/B pairs move the same way,
    and each moves by more than the A/A spread; inconclusive: all 10 have a value otherwise;
    no verdict: a pair lacks a value, or the metric is excluded on the platform."""
    if exclusion:
        metric.update(verdict="no verdict", reason=exclusion)
        return
    as_difference = metric["representation"] == "difference"
    complete = [[a, b] for a, b in metric["aa"] + metric["ab"] if a is not None and b is not None]
    if (len(metric["aa"]) != PAIRS["aa"] or len(metric["ab"]) != PAIRS["ab"]
            or len(complete) != PAIRS["aa"] + PAIRS["ab"]):
        metric.update(verdict="no verdict", reason=(
            f"{len(complete)} of {PAIRS['aa'] + PAIRS['ab']} pairs have a value"))
        return
    signed = [b - a if as_difference else math.log(b / a) for a, b in metric["ab"]]
    upward = all(each > 0 for each in signed)
    if ((upward or all(each < 0 for each in signed))
            and all(abs(each) > metric["spread"] for each in signed)):
        verdict = "improved" if upward == higher_is_better else "regressed"
    else:
        verdict = "inconclusive"
    metric.update(verdict=verdict, reason=None)


def wrapped(metric):
    """The observations of a cross-process latency that are unsigned wraps, not numbers."""
    if not metric["label"].startswith(CROSS_PROCESS):
        return 0
    return sum(1 for pair in metric["aa"] + metric["ab"] for value in pair
               if value is not None and value * 1000 >= WRAP_NS)


def call_kind(note):
    """aa or ab, from a record's note: "ci-compare <platform> <aa|ab> call <n>, benchmarks
    <head>"."""
    return note.split()[2]


def library_arm(record):
    """The library arm of a record: B only for the B sample of an A/B call. Both samples of an
    A/A call run arm A, whatever arm label the runner gives them."""
    return "B" if call_kind(record["note"]) == "ab" and record["arm"] == "B" else "A"


def median_fence_percent(samples):
    values = [value for value in map(runner.fence_percent, samples) if value is not None]
    return statistics.median(values) if values else None


def report_cell(entry, records_path, platform, null):
    """The comparable pairs, invalid samples, primary metrics with their verdicts, and other
    headline metrics of one cell."""
    records = runner.load_records(records_path)
    pairs = {"aa": [], "ab": []}
    by_call = {}
    for record in records:
        by_call.setdefault(record["note"], []).append(record)
    for note, call_records in by_call.items():
        pairs[call_kind(note)] += runner.comparable_pairs(call_records)
    report = {
        "id": entry["id"], "state": entry["state"], "null": null,
        "pairs": {kind: len(pairs[kind]) for kind in pairs},
        "invalid": {arm: sum(1 for record in records if library_arm(record) == arm
                             and not record["outcome"]["valid"]) for arm in "AB"},
        "fence_percent": None, "primary": [], "other": [],
    }
    if entry["id"].startswith("w1.") and pairs["ab"]:
        report["fence_percent"] = {
            arm: median_fence_percent([pair[index] for pair in pairs["ab"]])
            for index, arm in enumerate("AB")}
    tables = {kind: metric_table(pairs[kind]) for kind in pairs}
    always_difference = {label: flag for table in tables.values()
                         for label, (flag, _) in table.items()}
    # Every scheduled primary metric gets an entry: a cell that was not run, or that recorded
    # nothing, counts its primary metrics as `no verdict`.
    primary = primary_metrics(entry["id"])
    for label, higher_is_better in primary:
        metric = evaluate(label, always_difference.get(label, True),
                          tables["aa"].get(label, (True, []))[1],
                          tables["ab"].get(label, (True, []))[1])
        metric["higher_is_better"] = higher_is_better
        give_verdict(metric, higher_is_better, verdict_exclusion(label, platform))
        if not records:
            metric["reason"] = entry["state"]
        report["primary"].append(metric)
    for label, flag in always_difference.items():
        if label in dict(primary):
            continue
        metric = evaluate(label, flag, tables["aa"].get(label, (flag, []))[1],
                          tables["ab"].get(label, (flag, []))[1])
        if runner.is_single_shutdown_call(label, metric["aa"] + metric["ab"]):
            continue
        metric["wrapped"] = wrapped(metric)
        report["other"].append(metric)
    return report


def compiler_cache(output, arm):
    """Hits, misses and errors of one arm's build, from sccache's JSON statistics or ccache's
    text; None when the job recorded none."""
    sccache, ccache = output / f"cache-{arm}.json", output / f"cache-{arm}.txt"
    if sccache.is_file():
        try:
            stats = load_json(sccache)["stats"]
            return {"hits": sum(stats["cache_hits"]["counts"].values()),
                    "misses": sum(stats["cache_misses"]["counts"].values()),
                    "errors": sum(stats["cache_errors"]["counts"].values())
                    + stats["cache_read_errors"] + stats["cache_write_errors"]}
        except (ValueError, KeyError, TypeError, AttributeError) as error:
            return {"unreadable": f"sccache statistics: {error!r}"}
    if ccache.is_file():
        text = ccache.read_text(encoding="utf-8", errors="replace")
        hits = re.findall(r"^cache hit \((?:direct|preprocessed)\)\s+(\d+)", text, re.M)
        misses = re.findall(r"^cache miss\s+(\d+)", text, re.M)
        if len(hits) != 2 or len(misses) != 1:
            return {"unreadable": "ccache statistics in an unknown format"}
        return {"hits": sum(map(int, hits)), "misses": int(misses[0]), "errors": None}
    return None


def classify(args, job, context):
    """(status, reason, level-2 fault) of the platform (plan section 15.9). Red means that
    level 2 could not build the pushed commit, or could not produce its report; never a
    number."""
    if args.arm_b == "failure":
        return ("fault", "arm B, the pushed commit, did not build: the commit is broken, "
                "incompatible with level 2's toolchain, or level 2's build invocation failed",
                True)
    if args.arm_b != "success":
        return "fault", f"arm B, the pushed commit, {ARM_OUTCOMES[args.arm_b]}", True
    if args.arm_a != "success":
        unchanged = not (context["benchmarks_changed"] or context["build_definition_changed"])
        return ("not_compared", "base_build_failed" + (
            ": the overlay left base unchanged" if unchanged else
            ": head's benchmarks do not build against the base library"), False)
    if job is None:
        return "fault", "the driver left no job.json", True
    if job["state"] == "error":
        return "fault", "the driver failed: " + " ".join(job["error"].split()), True
    if job["state"] == "running":
        if time.time() >= job["deadline"]:
            return "stopped", "deadline", False
        return "fault", "the driver ended before its deadline without its completion mark", True
    if job["stop"] is not None:
        reason = job["stop"]["reason"]
        # Without an A/B record nothing was compared, whatever the A/A calls recorded.
        if reason != "deadline" and not any(
                call["records"] for cell in job["cells"] for call in cell["calls"]
                if call["kind"] == "ab"):
            return ("not_compared",
                    "configuration_differs" if reason == "configuration_differs" else
                    "no_records", False)
        return "stopped", reason, False
    return "compared", None, False


def command_report(args):
    context = load_json(args.context)
    job_path = args.output / "job.json"
    job = load_json(job_path) if job_path.is_file() else None
    status, reason, fault = classify(args, job, context)
    identical = job and job["identical_executables"]
    cells = []
    for entry in (job["cells"] if job else []):
        if job["state"] != "finished" and entry["state"] in ("pending", "running"):
            entry["state"] = ("not_run: " if entry["state"] == "pending" else "stopped: ") + (
                reason or "the driver ended")
        null = not context["library_changed"] or bool(
            identical and identical[workload_of(entry["id"])])
        cells.append(report_cell(entry, args.output / "records" / f"{entry['id']}.jsonl",
                                 args.platform, null))
    records = [record for path in (args.output / "records").glob("*.jsonl")
               for record in runner.load_records(path)]
    counts = {verdict: 0 for verdict in ("regressed", "improved", "inconclusive", "no verdict")}
    null = {"eligible": 0, "verdicts": 0}
    for cell in cells:
        for metric in cell["primary"]:
            counts[metric["verdict"]] += 1
            if cell["null"] and metric["verdict"] != "no verdict":
                null["eligible"] += 1
                null["verdicts"] += metric["verdict"] in ("regressed", "improved")
    cache = {arm: compiler_cache(args.output, arm.lower()) for arm in "AB"}
    defect = None
    if not context["library_changed"] and (cache["B"] or {}).get("misses"):
        defect = (f"arm B missed {cache['B']['misses']} compiles although the library and the "
                  "build definition are unchanged, so every arm B compile should have hit")
    summary = {
        "format": SUMMARY_FORMAT, "platform": args.platform, "attempt": args.attempt,
        "status": status, "reason": reason, "level2_fault": fault,
        "stop_message": job["stop"]["message"] if job and job["stop"] else None,
        "protocol": job and job["protocol"], "rule": RULE, "context": context,
        "provenance": {
            "image": args.image,
            "compiler": records[0]["build"]["compiler"] if records else None,
            "cpu_model": job and job["cpu_model"],
            "cpus": records[0]["host"]["cpus_effective"] if records else None,
            "host_os": records[0]["host"]["os"] if records else None},
        "cache": {"A": cache["A"], "B": cache["B"], "defect": defect},
        "identical_executables": identical,
        "cells": cells,
        "not_run_budget": sum(1 for cell in cells if cell["state"] == "not_run: budget"),
        "counts": counts,
        "null": null if any(cell["null"] for cell in cells) else None,
    }
    write_json(args.output / "summary.json", summary)
    write_text(args.output / "summary.md", summary_markdown(summary))
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as output:
            output.write(f"level2_fault={'true' if fault else 'false'}\n")
    print(f"{args.platform}: {status_text(summary)}")


def number(value):
    return f"{value:.4g}"


def comparison_text(metric):
    comparison = metric["comparison"]
    if metric.get("wrapped"):
        return f"invalid: {metric['wrapped']} observations are unsigned wraps"
    if comparison is None:
        return "unavailable"
    if metric["representation"] == "ratio":
        text = (f"B/A {number(comparison['median'])} "
                f"[{number(comparison['min'])}, {number(comparison['max'])}]")
    else:
        text = (f"A {number(comparison['median_a'])}, B {number(comparison['median_b'])}, "
                f"B-A {number(comparison['median'])} "
                f"[{number(comparison['min'])}, {number(comparison['max'])}]")
    text += f" over {comparison['n']} pairs"
    if metric["spread"] is None:
        return text + "; A/A spread unavailable"
    spread = (number(metric["spread"]) if metric["representation"] == "difference"
              else "x" + number(math.exp(metric["spread"])))
    return f"{text}; A/A spread {spread}"


def status_text(summary):
    """The status of a platform, from its summary or its entry in run.json."""
    shortfall = (f"; {summary['not_run_budget']} cells not run for the budget"
                 if summary.get("not_run_budget") else "")
    if summary["status"] == "compared":
        return "compared" + shortfall
    if summary["status"] == "stopped":
        return f"compared in part, stopped: {summary['reason']}{shortfall}"
    if summary["status"] == "not_compared":
        return f"not compared: {summary['reason']}"
    return f"level-2 fault (red): {summary['reason']}"


def cache_text(cache):
    if cache is None:
        return "not recorded"
    if "unreadable" in cache:
        return cache["unreadable"]
    errors = "" if cache["errors"] is None else f", {cache['errors']} errors"
    return f"{cache['hits']} hits, {cache['misses']} misses{errors}"


def push_range(context):
    head = context["head"][:12]
    if context["event"] != "push":
        return f"`{context['base'][:12]}..{head}` (the head's first parent, {context['event']})"
    fallback = " (fallback base: the head's first parent)" if context["fallback_base"] else ""
    return f"`{context['base'][:12]}..{head}`{fallback}"


def summary_markdown(summary):
    """The job summary of one platform (plan section 15.7)."""
    context, provenance = summary["context"], summary["provenance"]
    platform = summary["platform"]

    def yes_no(flag):
        return "yes" if flag else "no"

    lines = [f"### Benchmark comparison: {PLATFORM_NAMES[platform]}", "", WORDING, ""]
    if not context["library_changed"]:
        lines += [f"**{NULL_WORDING}**", ""]
    identical = summary["identical_executables"]
    lines += [
        "| | |", "|---|---|",
        f"| Status | {status_text(summary)} |",
        f"| Push range | {push_range(context)} |",
        f"| Library changed | {yes_no(context['library_changed'])} |",
        f"| Build definition changed | {yes_no(context['build_definition_changed'])} |",
        f"| Image | {provenance['image']} |",
        f"| Compiler | {provenance['compiler'] or 'unknown'} |",
        f"| CPU | {provenance['cpu_model'] or 'unknown'}, C = {provenance['cpus'] or 'unknown'} |",
        f"| Compiler cache | arm A: {cache_text(summary['cache']['A'])}; "
        f"arm B: {cache_text(summary['cache']['B'])} |",
    ]
    if summary["cache"]["defect"]:
        lines.append(f"| Cache defect | {summary['cache']['defect']} |")
    if identical is not None:
        same = [workload for workload, flag in identical.items() if flag]
        lines.append("| Byte-identical executables | "
                     + (", ".join(same) + " (null test for their cells)" if same else "none")
                     + " |")
    lines += [f"| Protocol | {summary['protocol'] or 'unknown'} |", ""]
    if summary["stop_message"]:
        lines += ["Why the platform stopped (the hard stop, or the runner's last output):", "",
                  "```text", summary["stop_message"], "```", ""]
    notes = [
        "Sintra is header-only, so a library change recompiles the executables, and inlining "
        "and code placement can move timings without a change of the measured path. A verdict "
        "says that head's binaries were faster or slower here, not why.",
        "A W1 row's rate and CPU include the time its sender waits in processing fences, shown "
        "as its fence share: where that share is high, its verdicts mostly reflect the "
        "fence-paced cycle and the readers' wait policy, not the publish path.",
        "W2 rows: read `rtt` together with `actual_gap` (plan section 5.2). A difference below "
        "one poll period is consistent with a phase effect, but phase locking can also hide a "
        "real change of that size.",
        "Cross-process latencies (`slowest_from_send`) rest on a common clock that is not "
        "established here: they are listed below, without a verdict.",
    ]
    if context["build_definition_changed"]:
        notes.insert(0, "The build definition changed as well: a verdict can come from the "
                     "library or from the build definition.")
    if platform != "linux":
        notes.append(
            "Outside Linux the readers mostly wait in their poll regime: the "
            "`leaf_to_coordinator` W2 cells sit in it, and the W1 rows are mostly fence time, "
            "so their verdicts here mostly reflect the readers' wait policy.")
    lines += [f"- {note}" for note in notes] + [""]
    if summary["cells"]:
        lines += ["| # | Cell | Pairs A/B, A/A | Invalid A, B | State | Primary metrics |",
                  "|---|---|---|---|---|---|"]
        for index, cell in enumerate(summary["cells"], 1):
            name = f"`{cell['id']}`"
            if cell["null"] and context["library_changed"]:
                name += " (null test: identical executables)"
            if cell["fence_percent"]:
                shares = [f"{arm} {cell['fence_percent'][arm]:.0f} %"
                          for arm in "AB" if cell["fence_percent"][arm] is not None]
                name += "<br>fence share " + ", ".join(shares)
            if cell["id"].startswith("w2."):
                name += "<br>W2 reading rule"
            if any(metric["comparison"] for metric in cell["primary"]):
                metrics = "<br>".join(
                    f"**{metric['verdict']}** {metric['label']}: {comparison_text(metric)}"
                    + (f" ({metric['reason']})" if metric["reason"] else "")
                    for metric in cell["primary"])
            else:
                metrics = f"**no verdict** for its {len(cell['primary'])} primary metrics"
            lines.append(
                f"| {index} | {name} | {cell['pairs']['ab']}, {cell['pairs']['aa']} | "
                f"{cell['invalid']['A']}, {cell['invalid']['B']} | {cell['state']} | "
                f"{metrics or '-'} |")
        lines += ["", "<details><summary>Every headline metric</summary>", "", "```text"]
        for cell in summary["cells"]:
            lines.append(cell["id"])
            for metric in cell["primary"] + cell["other"]:
                lines.append(f"  {metric['label']:30} {comparison_text(metric)}")
        lines += ["```", "", "</details>", ""]
    return "\n".join(lines) + "\n"


# --- collect -----------------------------------------------------------------------

def checked_summary(path, platform):
    """A platform's summary.json, read as data and checked against its schema."""
    summary = load_json(path)
    problems = []
    if summary.get("format") != SUMMARY_FORMAT or summary.get("platform") != platform:
        problems.append("format or platform")
    for key, kind in (("attempt", int), ("status", str), ("counts", dict), ("cells", list),
                      ("context", dict), ("provenance", dict), ("level2_fault", bool)):
        if not isinstance(summary.get(key), kind):
            problems.append(key)
    if problems:
        raise ValueError(f"{path} is not a {SUMMARY_FORMAT} summary of {platform}: "
                         + ", ".join(problems))
    return summary


def history_record(summary):
    """What the history keeps of a platform's summary: per cell the primary metrics with
    their per-pair values (at full precision, so that the rule re-applied to them gives the
    reported verdict) and verdicts, the missing-data reasons and the provenance. The other
    metrics, the fence shares and the cache statistics stay in the artifacts; the context is
    the run's, in run.json."""
    record = {key: value for key, value in summary.items() if key not in ("cache", "context")}
    record["format"] = HISTORY_FORMAT
    record["cells"] = [{key: value for key, value in cell.items()
                        if key not in ("other", "fence_percent")} for cell in summary["cells"]]
    return record


def command_collect(args):
    context = load_json(args.context)
    pattern = re.compile(
        rf"benchmark-compare-({'|'.join(PLATFORMS)})-{context['run_id']}-(\d+)")
    found = {}
    for directory in (args.artifacts.iterdir() if args.artifacts.is_dir() else []):
        match = pattern.fullmatch(directory.name)
        if match and (directory / "summary.json").is_file():
            found.setdefault(match.group(1), {})[int(match.group(2))] = (
                checked_summary(directory / "summary.json", match.group(1)))
    args.output.mkdir(parents=True, exist_ok=True)
    platforms = {}
    for platform in PLATFORMS:
        summaries = found.get(platform)
        if not summaries:
            platforms[platform] = {"status": "no_report", "reason": context["reason"]}
            continue
        # The summaries come from the highest attempt present; the null-test counts from the
        # first attempt that reported, so that a re-run cannot replace a false call.
        attempt, first = max(summaries), min(summaries)
        summary = summaries[attempt]
        platforms[platform] = {
            "attempt": attempt, "status": summary["status"], "reason": summary["reason"],
            "not_run_budget": summary["not_run_budget"], "counts": summary["counts"],
            "regressed": [f"{cell['id']}: {metric['label']}" for cell in summary["cells"]
                          for metric in cell["primary"] if metric["verdict"] == "regressed"],
            "improved": [f"{cell['id']}: {metric['label']}" for cell in summary["cells"]
                         for metric in cell["primary"] if metric["verdict"] == "improved"],
            "null": summaries[first]["null"] and dict(
                summaries[first]["null"], attempt=first,
                protocol=summaries[first]["protocol"],
                image=summaries[first]["provenance"]["image"],
                compiler=summaries[first]["provenance"]["compiler"]),
            "image": summary["provenance"]["image"],
            "compiler": summary["provenance"]["compiler"],
        }
        write_json(args.output / f"{platform}.json", history_record(summary))
    run = {"format": HISTORY_FORMAT, "collect_attempt": args.attempt,
           **{key: context[key] for key in (
               "run_id", "run_number", "event", "base", "head", "fallback_base", "reason",
               "library_changed", "build_definition_changed", "benchmarks_changed", "trees")},
           "platforms": platforms}
    write_json(args.output / "run.json", run)
    write_text(args.output / "summary.md", combined_markdown(run, args))
    print(f"collected run {context['run_id']}: " + ", ".join(
        f"{platform} {entry['status']}" for platform, entry in platforms.items()))


def combined_markdown(run, args):
    lines = [f"### Benchmark comparison, run #{run['run_number']}", "", WORDING, ""]
    if run["base"] is None:
        lines += [f"Not compared: {run['reason']}.", ""]
    else:
        lines += [f"Push range {push_range(run)}; library changed: "
                  f"{'yes' if run['library_changed'] else 'no'}; build definition changed: "
                  f"{'yes' if run['build_definition_changed'] else 'no'}.", ""]
    if run["base"] is not None and not run["library_changed"]:
        lines += [f"**{NULL_WORDING}**", ""]
    lines += ["| Platform | Status | Regressed | Improved | Attempt |", "|---|---|---|---|---|"]
    for platform, entry in run["platforms"].items():
        if "attempt" not in entry:
            lines.append(f"| {PLATFORM_NAMES[platform]} | no report | | | |")
            continue
        status = status_text(entry)
        lines.append(f"| {PLATFORM_NAMES[platform]} | {status} | "
                     f"{'<br>'.join(entry['regressed']) or '-'} | "
                     f"{'<br>'.join(entry['improved']) or '-'} | {entry['attempt']} |")
    url = f"{args.server_url}/{args.repository}"
    lines += ["", f"[Workflow run]({url}/actions/runs/{run['run_id']}) · "
              f"[History index]({url}/blob/{HISTORY_BRANCH}/index.md)", ""]
    return "\n".join(lines) + "\n"


# --- archive -----------------------------------------------------------------------

def git(work, *arguments, environment=None, data=None):
    return subprocess.run(["git", "-C", str(work), *arguments], input=data,
                          capture_output=True, env=environment)


def checked_git(work, *arguments, environment=None, data=None):
    """The output of a git command, as bytes; a failure raises."""
    result = git(work, *arguments, environment=environment, data=data)
    if result.returncode != 0:
        raise RuntimeError(f"git {arguments[0]} failed: "
                           + result.stderr.decode(errors="replace").strip())
    return result.stdout


def remote_environment():
    """The environment of the git commands that reach the remote (ls-remote, fetch, push), and
    of no other command. HISTORY_TOKEN leaves this process's environment here. With it, git
    reads an authorization header from the environment, so the token is neither stored in a
    configuration file nor visible in a command line, and the log masks the derived
    credential."""
    token = os.environ.pop("HISTORY_TOKEN", None)
    environment = dict(os.environ)
    if token:
        credential = base64.b64encode(f"x-access-token:{token}".encode()).decode()
        print(f"::add-mask::{credential}", flush=True)
        environment.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="http.extraheader",
                           GIT_CONFIG_VALUE_0=f"AUTHORIZATION: basic {credential}")
    return environment


def format_version(text):
    """The version of a history file's format marker; None when it has none."""
    match = re.search(r"sintra-bench-ci-history/(\d+)", text)
    return int(match.group(1)) if match else None


def check_history_format(work):
    """Refuses a branch that holds a file of a newer or unknown format: this job would
    rewrite what it does not understand."""
    known = format_version(HISTORY_FORMAT)
    for path in sorted(work.rglob("*")):
        if ".git" in path.relative_to(work).parts or not path.is_file():
            continue
        version = format_version(path.read_text(encoding="utf-8", errors="replace"))
        if version is None or version > known:
            raise RuntimeError(
                f"history-write failure: {path.relative_to(work).as_posix()} has "
                f"{'no format marker' if version is None else f'format version {version}'}; "
                f"this archive job understands {HISTORY_FORMAT}")


def pack_size(work):
    """The size of a pack of exactly the objects reachable from HEAD. A failure of either
    command raises, so that a partial pack is never taken for the history's size."""
    objects = checked_git(work, "rev-list", "--objects", "HEAD")
    return len(checked_git(work, "pack-objects", "--stdout", "-q", data=objects))


def history_readme(budget):
    return f"""<!-- {HISTORY_FORMAT} -->
# Benchmark comparison history

The history of level 2 of the benchmark CI: `.github/workflows/benchmark-compare.yml` on
`master`; see `benchmarks/README.md`, "Continuous integration", and `docs/benchmark_plan.md`,
section 15. Only that workflow's archive job writes this branch, and it never force-pushes.
Its numbers are reports, never gates.

- `index.md`: one line per push run of `master`, newest first, with the observed false-call
  rate of the null tests and the history's size.
- `runs/<run ID>/run.json`: the run's commits, base kind, change flags and library tree IDs,
  and per platform its status, verdict counts, the attempt used and the null-test counts of
  the first attempt that reported.
- `runs/<run ID>/<platform>.json`: that platform's summary. Per cell: the comparable pairs,
  the invalid samples, any stop, and per primary metric its representation, the values of
  every A/A and A/B pair (so that the rule can be re-applied), and its verdict or why it has
  none; also the job's provenance (image, compiler, CPU model, C). The records, the other
  metrics and the build logs stay in the run's artifacts, which GitHub keeps for 90 days.

Every file carries the format `{HISTORY_FORMAT}`. The archive job refuses, with a
history-write failure, a branch whose format is newer than it understands.

## Budget

The history may grow to {budget / 1e6:g} MB, measured as the size of a pack of exactly the
objects reachable from the proposed tip. A run adds roughly 20 KB, so the budget holds about
400 runs. The index warns from {HISTORY_WARNING_SHARE:.0%} of it. A run that would pass the
budget is not archived: the archive job fails, the job summaries and artifacts are unaffected,
and the owner decides. The remedies, none of them a force push:

- archive under a new branch name (`HISTORY_BRANCH` in `benchmarks/ci_compare.py`), which
  starts a new budget but does not bound the repository's total history;
- delete this branch.
"""


def false_call_table(runs):
    """Per platform and protocol version: the verdicts over the verdict-eligible primary
    metrics of the null tests, historical (across images and compilers) and for the image and
    compiler of the platform's latest run."""
    lines = []
    for platform in PLATFORMS:
        reported = [run for run in runs if "attempt" in run["platforms"][platform]]
        if not reported:
            continue
        latest = reported[-1]["platforms"][platform]
        current = (latest["image"], latest["compiler"])
        nulls = [run["platforms"][platform]["null"] for run in reported
                 if run["platforms"][platform]["null"]]
        for protocol in dict.fromkeys(null["protocol"] for null in nulls):
            of_protocol = [null for null in nulls if null["protocol"] == protocol]
            matching = [null for null in of_protocol
                        if (null["image"], null["compiler"]) == current]

            def rate(nulls):
                eligible = sum(null["eligible"] for null in nulls)
                verdicts = sum(null["verdicts"] for null in nulls)
                if not eligible:
                    return "unknown: no null test yet"
                return f"{verdicts} of {eligible} ({len(nulls)} null tests)"

            lines.append(f"| {PLATFORM_NAMES[platform]} | `{protocol}` | {rate(of_protocol)} | "
                         f"{rate(matching)} |")
    return lines


def history_index(runs, size, budget, repository):
    """index.md: the false-call rate, the size, and one line per push run, newest first.
    `runs` is ordered by run number."""
    share = size / budget
    lines = [f"<!-- {HISTORY_FORMAT} -->", "# Benchmark comparison history", "",
             "One line per push run of `master`; see README.md for how to read the files.", "",
             f"History size: {size / 1e3:.0f} KB of the {budget / 1e6:g} MB budget "
             f"({share:.1%})."]
    if share >= HISTORY_WARNING_SHARE:
        lines.append(f"**Warning: the history has reached {share:.0%} of its budget. See the "
                     "remedies in README.md.**")
    lines += ["", "## False calls in null tests", "",
              "A null test compares identical libraries (the push left the library unchanged) "
              "or byte-identical executables, so every `regressed` or `improved` in it is a "
              "false call. Shown: false calls over the verdict-eligible primary metrics, from "
              "the first attempt of each run that reported. The historical column spans every "
              "image and compiler that the runners had, so it mixes environments; the other "
              "column counts only the image and compiler of the platform's latest run."]
    table = false_call_table(runs)
    if table:
        lines += ["", "| Platform | Protocol | Historical | Current image and compiler |",
                  "|---|---|---|---|", *table]
    else:
        lines += ["", "The hosted false-call rate is unknown: no null test has reported yet."]
    lines += ["", "## Runs", "",
              "Per platform: regressed / improved / inconclusive / no verdict, or the status. "
              "`gap`: the library of this run's base differs from that of the previous indexed "
              "head, so a range of `master` is not covered.", "",
              "| Run | Push range | Library | Build definition | "
              + " | ".join(PLATFORM_NAMES.values()) + " | Gap |",
              "|---" * (5 + len(PLATFORMS)) + "|"]
    url = f"https://github.com/{repository}"
    previous_head = None
    rows = []
    for run in runs:
        gap = (previous_head is not None and run["trees"]["base"] is not None
               and run["trees"]["base"] != previous_head)
        previous_head = run["trees"]["head"]
        if run["base"] is None:
            span = (f"[{run['head'][:12]}]({url}/commit/{run['head']}), "
                    f"not compared: {run['reason']}")
        else:
            span = (f"[{run['base'][:12]}..{run['head'][:12]}]"
                    f"({url}/compare/{run['base']}...{run['head']})")
            if run["fallback_base"]:
                span += " fallback base"
        cells = []
        for platform in PLATFORMS:
            entry = run["platforms"][platform]
            if "attempt" not in entry:
                cells.append("no report")
                continue
            counts = entry["counts"]
            text = " / ".join(str(counts[key]) for key in
                              ("regressed", "improved", "inconclusive", "no verdict"))
            if entry["status"] == "stopped":
                text += f", stopped: {entry['reason']}"
            elif entry["status"] != "compared":
                text = status_text(entry)
            if entry["not_run_budget"]:
                text += f", {entry['not_run_budget']} cells not_run: budget"
            if entry["attempt"] > 1:
                text += f" (attempt {entry['attempt']})"
            cells.append(text)
        rows.append(f"| [#{run['run_number']}]({url}/actions/runs/{run['run_id']}) | {span} | "
                    f"{'changed' if run['library_changed'] else 'unchanged'} | "
                    f"{'changed' if run['build_definition_changed'] else 'unchanged'} | "
                    + " | ".join(cells) + f" | {'gap' if gap else ''} |")
    return "\n".join(lines + rows[::-1]) + "\n"


def write_history(work, run_files, run_id, size, budget, repository):
    """Adds or replaces the run's directory, then regenerates the README and the index."""
    directory = work / "runs" / str(run_id)
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    for path in run_files:
        write_json(directory / path.name, load_json(path))
    runs = sorted((load_json(path) for path in (work / "runs").glob("*/run.json")),
                  key=lambda run: run["run_number"])
    write_text(work / "README.md", history_readme(budget))
    write_text(work / "index.md", history_index(runs, size, budget, repository))


def latest_collect_artifact(directory, run_id):
    """The collect artifact of the highest attempt that the archive job downloaded."""
    pattern = re.compile(rf"benchmark-compare-run-{run_id}-(\d+)")
    found = {int(match.group(1)): path for path in directory.iterdir()
             if (match := pattern.fullmatch(path.name))}
    if not found:
        raise RuntimeError(f"no collect artifact of run {run_id} in {directory}")
    return found[max(found)]


def command_archive(args):
    artifact = latest_collect_artifact(args.run_artifacts, args.run_id)
    run = load_json(artifact / "run.json")
    if run.get("format") != HISTORY_FORMAT or run.get("run_id") != args.run_id:
        raise RuntimeError(f"{artifact / 'run.json'} is not the {HISTORY_FORMAT} file of run "
                           f"{args.run_id}")
    run_files = sorted(artifact.glob("*.json"))
    for path in run_files:
        if load_json(path).get("format") != HISTORY_FORMAT:
            raise RuntimeError(f"{path} is not a {HISTORY_FORMAT} file")
    environment = remote_environment()
    committer = dict(os.environ, GIT_AUTHOR_NAME=BOT_NAME, GIT_AUTHOR_EMAIL=BOT_EMAIL,
                     GIT_COMMITTER_NAME=BOT_NAME, GIT_COMMITTER_EMAIL=BOT_EMAIL)
    ref = f"refs/heads/{HISTORY_BRANCH}"
    work = Path(tempfile.mkdtemp(prefix="benchmark-history-"))
    try:
        checked_git(work, "init", "-q")
        for attempt in range(1, PUSH_ATTEMPTS + 1):
            exists = checked_git(work, "ls-remote", "--heads", args.remote, ref,
                                 environment=environment).strip()
            if exists:
                checked_git(work, "fetch", "-q", "--no-tags", args.remote,
                            f"+{ref}:refs/remotes/history", environment=environment)
                checked_git(work, "checkout", "-q", "--force", "--detach",
                            "refs/remotes/history")
            else:
                # The first run creates the branch as an orphan.
                checked_git(work, "checkout", "-q", "--force", "--orphan", f"orphan-{attempt}")
                checked_git(work, "rm", "-rfq", "--ignore-unmatch", ".")
            checked_git(work, "clean", "-fdxq")
            check_history_format(work)
            message = f"Archive benchmark comparison run #{run['run_number']} ({args.run_id})"
            # The index states the size, so the commit is measured, given its index, and
            # measured again.
            size = 0
            for _ in range(2):
                write_history(work, run_files, args.run_id, size, args.budget_bytes,
                              args.repository)
                checked_git(work, "add", "-A")
                if size == 0:
                    checked_git(work, "commit", "-q", "-m", message, environment=committer)
                else:
                    checked_git(work, "commit", "-q", "--amend", "-m", message,
                                environment=committer)
                size = pack_size(work)
            print(f"history size {size} bytes of the budget {args.budget_bytes}")
            if size > args.budget_bytes:
                raise RuntimeError(
                    f"history-write failure: the history would grow to {size} bytes, past its "
                    f"budget of {args.budget_bytes}. Nothing was pushed; see the remedies in "
                    f"the branch's README.md.")
            if size >= HISTORY_WARNING_SHARE * args.budget_bytes:
                print(f"::warning::The benchmark history has reached "
                      f"{size / args.budget_bytes:.0%} of its budget; see its README.md.")
            pushed = git(work, "push", "-q", args.remote, f"HEAD:{ref}",
                         environment=environment)
            if pushed.returncode == 0:
                print(f"archived run {args.run_id} in {HISTORY_BRANCH} (attempt {attempt})")
                return
            print(f"push attempt {attempt} was rejected: "
                  + pushed.stderr.decode(errors="replace").strip())
            if attempt < PUSH_ATTEMPTS:
                time.sleep(random.uniform(5, 30))
        raise RuntimeError(f"history-write failure: {PUSH_ATTEMPTS} push attempts failed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


# --- Entry point ---------------------------------------------------------------------

def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="measure the CI cells against two built arms")
    run.add_argument("--platform", required=True, choices=PLATFORMS)
    run.add_argument("--context", required=True, type=Path, help="the setup job's context")
    run.add_argument("--arms", required=True, type=Path,
                     help="directory with arm-a/benchmarks and arm-b/benchmarks")
    run.add_argument("--deadline", required=True, type=float,
                     help="Unix time by which every priced cell must end")
    run.add_argument("--hard-stop", required=True, type=float,
                     help="Unix time at which the driver interrupts a call and stops")
    run.add_argument("--output", required=True, type=Path)
    run.add_argument("--workloads", type=Path,
                     default=Path(__file__).with_name("workloads_ci.json"))

    report = commands.add_parser("report", help="classify the job and write its summary")
    report.add_argument("--platform", required=True, choices=PLATFORMS)
    report.add_argument("--context", required=True, type=Path)
    report.add_argument("--output", required=True, type=Path)
    report.add_argument("--arm-a", required=True, help="outcome of the arm A build")
    report.add_argument("--arm-b", required=True, help="outcome of the arm B build")
    report.add_argument("--attempt", required=True, type=int)
    report.add_argument("--image", required=True, help="the runner image, as recorded")
    report.add_argument("--github-output", type=Path,
                        help="file to which level2_fault=true|false is appended")

    collect = commands.add_parser("collect", help="combine the platform summaries of a run")
    collect.add_argument("--context", required=True, type=Path)
    collect.add_argument("--artifacts", required=True, type=Path)
    collect.add_argument("--attempt", required=True, type=int)
    collect.add_argument("--output", required=True, type=Path)
    collect.add_argument("--repository", required=True)
    collect.add_argument("--server-url", default="https://github.com")

    archive = commands.add_parser("archive", help="add a run to the history branch")
    archive.add_argument("--run-artifacts", required=True, type=Path)
    archive.add_argument("--run-id", required=True, type=int)
    archive.add_argument("--remote", required=True)
    archive.add_argument("--repository", required=True)
    archive.add_argument("--budget-bytes", type=int, default=HISTORY_BUDGET_BYTES)
    return parser.parse_args()


def main():
    args = parse_arguments()
    {"run": command_run, "report": command_report, "collect": command_collect,
     "archive": command_archive}[args.command](args)


if __name__ == "__main__":
    main()
