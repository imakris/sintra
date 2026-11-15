#!/usr/bin/env python3
"""Paired runner for the Sintra benchmarks.

Runs the selected cells of a workloads file as A/B pairs with alternating order
(as an A/A session when --b is absent), appends one JSON record per sample to
the output, and prints the comparison summary and the axis tables. With
--calibrate it measures the windowed cells instead and writes a new workloads
file. See benchmarks/README.md.
"""

import argparse
import datetime
import fnmatch
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import select
import shutil
import signal
import statistics
import subprocess
import sys
import time

SCHEMA = "sintra-bench/1"
EXECUTABLES = {"broadcast": "sintra_broadcast_benchmark"}
CELL_PREFIXES = {"broadcast": "w1"}
TIMEOUT_BASE_S = 60         # a sample's timeout is this plus three times its planned duration
REPORT_WAIT_S = 10          # for the reports of every participant, after the coordinator's exit
EXIT_WAIT_S = 10            # for the participants to exit, on their own or after a kill
OBSERVE_INTERVAL_S = 0.25   # between looks at the participants while the coordinator runs
CALIBRATION_RUNS = 3
ROLES = ["coordinator", "coordinator_sender", "sender", "receivers"]
HAS_PROCFS = Path("/proc/self/cmdline").exists()


def fail(message):
    sys.exit(f"run_benchmarks: {message}")


# --- Workloads ---------------------------------------------------------------

def is_count(value, least=1):
    return isinstance(value, int) and not isinstance(value, bool) and value >= least


def is_positive_number(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and value > 0)


def validate_workloads(workloads):
    """Rejects a file that would silently run another experiment than it describes, or none."""
    problems = []
    try:
        open_loop = workloads["open_loop"]
        fractions = open_loop["rate_fractions"]
        if not is_positive_number(workloads["target_interval_s"]):
            problems.append("target_interval_s must be a finite, positive number")
        if not is_positive_number(open_loop["duration_s"]):
            problems.append("open_loop.duration_s must be a finite, positive number")
        if not fractions or not all(is_positive_number(fraction) for fraction in fractions):
            problems.append("open_loop.rate_fractions must be finite, positive numbers")
        if not all(is_count(count) for count in workloads["default_receivers"]):
            problems.append("default_receivers must be positive integers")
        for index, entry in enumerate(workloads["cells"]):
            where = f"cells[{index}]"
            if entry["workload"] not in EXECUTABLES:
                problems.append(f"{where}: unknown workload {entry['workload']!r}")
            if entry["origin"] not in ("coordinator", "leaf"):
                problems.append(f"{where}: origin must be coordinator or leaf")
            for key in ("receivers", "payload_bytes", "messages"):
                if not is_count(entry[key]):
                    problems.append(f"{where}: {key} must be a positive integer")
            for key in ("burst", "handler_delay_us"):
                if entry.get(key) is not None and not is_count(entry[key], least=0):
                    problems.append(f"{where}: {key} must be a non-negative integer")
            rates = entry.get("open_loop_rates")
            if rates is None:
                continue
            if not isinstance(rates, list) or len(rates) != len(fractions):
                problems.append(f"{where}: open_loop_rates needs one rate per rate fraction")
            elif not all(rate is None or is_positive_number(rate) for rate in rates):
                problems.append(f"{where}: a rate must be null or a finite, positive number")
    except (KeyError, TypeError) as error:
        problems.append(f"malformed entry or missing key: {error!r}")
    if not problems:
        ids = [cell["id"] for cell in expand_cells(workloads)]
        repeated = sorted({cell_id for cell_id in ids if ids.count(cell_id) > 1})
        if repeated:
            problems.append("cell IDs are not unique: " + ", ".join(repeated))
    if problems:
        fail("invalid workloads file:\n  " + "\n  ".join(problems))


def expand_cells(workloads):
    """Every defined cell: per entry, a windowed cell and one open-loop cell per rate fraction."""
    open_loop = workloads["open_loop"]
    cells = []
    for entry in workloads["cells"]:
        base = {
            "workload": entry["workload"],
            "origin": entry["origin"],
            "receivers": entry["receivers"],
            "payload_bytes": entry["payload_bytes"],
            "calibration": entry.get("calibration"),
            "burst": entry.get("burst"),
            "handler_delay_us": entry.get("handler_delay_us"),
            "default": entry["receivers"] in workloads["default_receivers"],
            "entry": entry,
        }
        prefix = CELL_PREFIXES[entry["workload"]]
        name = f"{entry['origin']}.r{entry['receivers']}.b{entry['payload_bytes']}"
        cells.append(dict(
            base, id=f"{prefix}.windowed.{name}", mode="windowed", messages=entry["messages"],
            offered_rate=None, duration_s=workloads["target_interval_s"]))
        # The measured messages of an open-loop cell are all i with i/rate < duration.
        rates = entry.get("open_loop_rates") or [None] * len(open_loop["rate_fractions"])
        for fraction, rate in zip(open_loop["rate_fractions"], rates):
            cells.append(dict(
                base, id=f"{prefix}.open_loop.{name}.x{round(fraction * 100)}", mode="open_loop",
                messages=rate and math.ceil(rate * open_loop["duration_s"]),
                offered_rate=rate, duration_s=open_loop["duration_s"]))
    return cells


def select_cells(cells, patterns):
    """The default grid, or every defined cell that matches one of the globs."""
    if not patterns:
        return [cell for cell in cells if cell["default"]]
    return [cell for cell in cells
            if any(fnmatch.fnmatchcase(cell["id"], pattern) for pattern in patterns)]


def write_workloads(workloads, path):
    """One line per setting and per cell, so that a calibration reads as a line diff."""
    head = "".join(f"  {json.dumps(key)}: {json.dumps(value)},\n"
                   for key, value in workloads.items() if key != "cells")
    cells = ",\n".join("    " + json.dumps(cell) for cell in workloads["cells"])
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write("{\n" + head + '  "cells": [\n' + cells + "\n  ]\n}\n")


def round_significant(value, digits=2):
    exponent = math.floor(math.log10(value)) - digits + 1
    if exponent < 0:
        return round(value, -exponent)
    return round(value / 10 ** exponent) * 10 ** exponent


# --- Ownership of a sample's processes ----------------------------------------

class Participants:
    """The processes of one sample that the runner owns: the coordinator it started, and that
    coordinator's leaves. A subclass establishes the ownership for its platform; nothing is
    ever signalled that is not identified as a participant of this sample."""

    def __init__(self, process):
        self.process = process

    def observe(self):
        """Retains the identity of the participants that are alive now."""

    def survivors(self):
        """The PIDs of the owned participants that are still alive."""
        raise NotImplementedError

    def kill_survivors(self):
        raise NotImplementedError

    def close(self):
        pass

    def wait_for_coordinator(self, timeout):
        """Whether the coordinator exited within the timeout."""
        deadline = time.monotonic() + timeout
        while True:
            self.observe()
            try:
                self.process.wait(
                    timeout=max(0, min(OBSERVE_INTERVAL_S, deadline - time.monotonic())))
                return True
            except subprocess.TimeoutExpired:
                if time.monotonic() >= deadline:
                    return False

    def wait_for_exit(self):
        """Waits for the participants to exit on their own; returns those that did not."""
        deadline = time.monotonic() + EXIT_WAIT_S
        while True:
            alive = self.survivors()
            if not alive or time.monotonic() >= deadline:
                return alive
            time.sleep(0.05)

    def terminate(self):
        """Kills the participants and waits for them; returns those it could not remove."""
        deadline = time.monotonic() + EXIT_WAIT_S
        while True:
            alive = self.survivors()
            if not alive or time.monotonic() >= deadline:
                return alive
            self.kill_survivors()
            time.sleep(0.05)


class Windows_participants(Participants):
    """Owns the coordinator's process tree through retained native handles.

    The tree starts from the handle of the coordinator the runner started and adds the handle
    of each descendant while it is alive. Termination addresses those handles: neither a
    recorded PID nor a reused one is ever signalled.
    """

    def __init__(self, process, process_tree):
        super().__init__(process)
        self.tree = process_tree(process)

    def observe(self):
        self.tree.refresh()

    def survivors(self):
        coordinator = [self.process.pid] if self.process.poll() is None else []
        return coordinator + self.tree.live_descendants()

    def kill_survivors(self):
        self.tree.terminate()

    def close(self):
        self.tree.close()


def load_windows_process_tree():
    """The test runner's Windows process tree, loaded by its path: the module is
    self-contained, and the test framework is not imported."""
    path = Path(__file__).resolve().parent.parent / "tests/runner/platform/windows_process_tree.py"
    if not path.is_file():
        fail(f"{path} is missing. On Windows the runner owns the benchmark's process tree "
             "through it, so it runs only from a checkout that has the tests directory.")
    spec = importlib.util.spec_from_file_location("windows_process_tree", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.Windows_process_tree


def command_line(pid):
    """The arguments of the process that has this PID now; empty when there is none."""
    if HAS_PROCFS:
        try:
            raw = Path(f"/proc/{pid}/cmdline").read_bytes()
        except OSError:
            return []
        return [argument.decode(errors="replace") for argument in raw.split(b"\0")]
    listing = subprocess.run(
        ["ps", "-ww", "-o", "command=", "-p", str(pid)],
        capture_output=True, text=True, check=False)
    return listing.stdout.split()


def has_exited(pidfd):
    return bool(select.select([pidfd], [], [], 0)[0])


class Posix_participants(Participants):
    """Owns the coordinator as the runner's child, and the leaves through their recorded PIDs.

    Leaves start their own sessions, so the coordinator's process group does not contain
    them. A recorded PID is signalled only while the process that has it is identified as a
    participant of this sample: its command line names this executable and this run ID. On
    Linux a pidfd pins the identified process while it is alive, so that the check and the
    signal refer to the same process whatever becomes of the PID. Elsewhere the identity is
    established immediately before the signal.
    """

    def __init__(self, process, run_dir, executable, run_id):
        super().__init__(process)
        self.run_dir = run_dir
        self.executable = executable.name
        self.run_id = run_id
        self.pins = hasattr(os, "pidfd_open")
        self.pinned = {}        # PID -> pidfd of a participant identified while it was alive
        self.examined = set()   # the recorded PIDs that pinning has looked at

    def recorded_pids(self):
        return [int(path.stem) for path in self.run_dir.glob("*.pid") if path.stem.isdigit()]

    def is_participant(self, pid):
        """Whether the process that has this PID now belongs to this sample."""
        arguments = command_line(pid)
        return self.run_id in arguments and any(
            Path(argument).name == self.executable for argument in arguments)

    def observe(self):
        for pid in self.recorded_pids() if self.pins else []:
            if pid in self.examined:
                continue
            self.examined.add(pid)
            try:
                pidfd = os.pidfd_open(pid)
            except ProcessLookupError:
                continue
            except OSError:
                self.pins = False   # no pidfd on this kernel: identify before each signal
                return
            # The descriptor pins one process. If that process is still alive after its
            # command line was read, the command line is its own.
            if self.is_participant(pid) and not has_exited(pidfd):
                self.pinned[pid] = pidfd
            else:
                os.close(pidfd)

    def survivors(self):
        self.observe()
        if self.pins:
            leaves = [pid for pid, pidfd in self.pinned.items() if not has_exited(pidfd)]
        else:
            leaves = [pid for pid in self.recorded_pids() if self.is_participant(pid)]
        coordinator = [self.process.pid] if self.process.poll() is None else []
        return sorted(set(coordinator + leaves))

    def kill_survivors(self):
        self.observe()
        try:
            if self.process.poll() is None:
                # The coordinator leads the process group that the runner started it in, and
                # that group ID cannot be reused while it is an unreaped child of the runner.
                os.killpg(self.process.pid, signal.SIGKILL)
            if self.pins:
                for pidfd in self.pinned.values():
                    if not has_exited(pidfd):
                        signal.pidfd_send_signal(pidfd, signal.SIGKILL)
            else:
                for pid in self.recorded_pids():
                    # The identity is established immediately before the signal.
                    if self.is_participant(pid):
                        os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass    # it exited meanwhile; the caller looks again

    def close(self):
        for pidfd in self.pinned.values():
            os.close(pidfd)


# --- Samples -----------------------------------------------------------------

def find_executable(build_dir, workload):
    name = EXECUTABLES[workload] + (".exe" if os.name == "nt" else "")
    for directory in (build_dir / "benchmarks", build_dir / "benchmarks" / "Release", build_dir):
        if (directory / name).is_file():
            return directory / name
    fail(f"{name} was not found under {build_dir}")


def read_build_facts(executable):
    result = subprocess.run(
        [str(executable), "--build-facts"], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def warmup_messages(cell):
    return max(64, cell["messages"] // 20)


def is_validation_only(cell, facts):
    """Whether the cell's options change the effective workload: a handler delay, or a burst
    other than the default one. The executable marks its records by the same rule."""
    default_burst = max(1, facts["message_ring_size"] // 4 // (cell["payload_bytes"] + 512))
    return bool(cell["handler_delay_us"]) or cell["burst"] not in (None, default_burst)


def cell_command(executable, cell, run_id):
    command = [
        str(executable), "--mode", cell["mode"], "--origin", cell["origin"],
        "--receivers", str(cell["receivers"]), "--bytes", str(cell["payload_bytes"]),
        "--messages", str(cell["messages"]), "--warmup", str(warmup_messages(cell)),
        "--run-id", run_id]
    for flag, key in (("--rate", "offered_rate"), ("--burst", "burst"),
                      ("--handler-delay-us", "handler_delay_us")):
        if cell[key] is not None:
            command += [flag, str(cell[key])]
    return command


def wait_for_reports(run_dir):
    """Waits for one report per participant that recorded its PID."""
    expected = len(list(run_dir.glob("*.pid")))
    deadline = time.monotonic() + REPORT_WAIT_S
    while len(list(run_dir.glob("*.bin"))) < expected and time.monotonic() < deadline:
        time.sleep(0.05)


def remove_run_directory(run_dir):
    """Windows can keep a file of a process that has just exited busy for a moment."""
    for _ in range(100):
        shutil.rmtree(run_dir, ignore_errors=True)
        if not run_dir.exists():
            return
        time.sleep(0.05)
    print(f"warning: could not remove {run_dir}", file=sys.stderr)


def unaggregated_record(cell, run_id, facts):
    """The record of a sample that left nothing to aggregate: it stalled or died before
    its processes wrote their reports."""
    return {
        "run_id": run_id,
        "build": facts,
        "cell": {
            "workload": cell["workload"], "mode": cell["mode"], "origin": cell["origin"],
            "receivers": cell["receivers"], "payload_bytes": cell["payload_bytes"],
            "offered_rate": cell["offered_rate"], "planned": cell["messages"],
            "warmup": warmup_messages(cell),
            "validation_only": is_validation_only(cell, facts),
        },
        "counts": None, "throughput": None, "latency": None, "cpu": None, "lifecycle": None,
        "outcome": {"missing_files": None, "valid": False, "reasons": [], "warnings": []},
    }


class Session:
    """The binaries, the host facts and the output of one runner invocation."""

    def __init__(self, args, cells):
        # Without --b, the A binaries run as both arms. A calibration is not an A/A session.
        self.aa = args.b is None and args.calibrate is None
        self.note = args.note
        self.keep = args.keep
        self.commits = {"A": args.a_commit, "B": args.b_commit if args.b else args.a_commit}
        self.process_tree = load_windows_process_tree() if os.name == "nt" else None
        self.executables = {}
        self.facts = {}
        build_dirs = {"A": args.a, "B": args.b or args.a}
        for workload in sorted({cell["workload"] for cell in cells}):
            for arm, build_dir in build_dirs.items():
                self.executables[arm, workload] = find_executable(build_dir, workload)
            facts = {arm: read_build_facts(self.executables[arm, workload]) for arm in build_dirs}
            if facts["A"] != facts["B"]:
                fail("the build facts of the two arms differ:\n"
                     f"A: {facts['A']}\nB: {facts['B']}")
            self.facts[workload] = facts["A"]
        concurrency = next(iter(self.facts.values()))["hardware_concurrency"]
        self.host = {
            "label": platform.node(),
            "os": platform.platform(),
            "hardware_concurrency": concurrency,
            "cpus_effective": args.cpus or concurrency,
        }
        self.id = f"{time.strftime('%Y%m%dT%H%M%S')}-{os.getpid()}"
        self.samples = 0
        self.runs_root = args.output.parent / (args.output.name + ".runs")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        self.output = args.output.open("a", encoding="utf-8", newline="\n")

    def close(self):
        self.output.close()
        if not self.keep and self.runs_root.is_dir() and not any(self.runs_root.iterdir()):
            self.runs_root.rmdir()

    def run_swarm(self, executable, cell, run_id, run_dir, timeout):
        """Runs one swarm to the coordinator's exit or to the timeout, and leaves no
        participant of it running, whatever interrupts the runner meanwhile.

        Returns the coordinator's exit code, whether the sample timed out, whether participants
        outlived a normal exit, and the PIDs of the participants that could not be removed.
        """
        with open(run_dir / "stdout.txt", "wb") as out, open(run_dir / "stderr.txt", "wb") as err:
            process = subprocess.Popen(
                cell_command(executable, cell, run_id), stdout=out, stderr=err,
                env=dict(os.environ, SINTRA_BENCHMARK_DIR=str(run_dir)),
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                start_new_session=os.name != "nt")
        participants = None
        try:
            if os.name == "nt":
                participants = Windows_participants(process, self.process_tree)
            else:
                participants = Posix_participants(process, run_dir, executable, run_id)
            timed_out = not participants.wait_for_coordinator(timeout)
            stray = False
            if not timed_out:
                wait_for_reports(run_dir)
                stray = bool(participants.wait_for_exit())
            remaining = participants.terminate()
            return process.returncode, timed_out, stray, remaining
        except BaseException:
            # An interrupt or a failure of the runner must not leave the swarm running.
            if participants is None:
                process.kill()
            elif participants.terminate():
                print(f"warning: participants of run {run_id} are still running; "
                      f"{run_dir} is kept", file=sys.stderr)
            elif not self.keep:
                remove_run_directory(run_dir)
            raise
        finally:
            if participants is not None:
                participants.close()

    def sample(self, arm, cell, pair_id=None, order=0):
        """Runs one sample, appends its record to the output and returns it."""
        self.samples += 1
        run_id = f"{self.id}-{self.samples:05d}"
        run_dir = self.runs_root / run_id
        run_dir.mkdir(parents=True)
        executable = self.executables[arm, cell["workload"]]
        facts = self.facts[cell["workload"]]
        ring_entries = len(list(Path(facts["ring_parent_dir"]).glob("sintra-*")))
        timeout = TIMEOUT_BASE_S + 3 * cell["duration_s"]
        started = time.time()
        begin = time.monotonic()
        exit_code, timed_out, stray, remaining = self.run_swarm(
            executable, cell, run_id, run_dir, timeout)
        wall = time.monotonic() - begin

        # A swarm that stalled in shutdown has already written its reports, so a timed-out
        # sample is aggregated too. One that stalled earlier has nothing to aggregate.
        reasons = []
        if timed_out:
            reasons.append("timeout")
        elif exit_code != 0:
            reasons.append("exit_code_nonzero")
        if remaining:
            reasons.append("cleanup_incomplete")
        aggregate = subprocess.run(
            [str(executable), "--aggregate", str(run_dir)],
            capture_output=True, text=True, check=False)
        if aggregate.returncode == 0:
            record = json.loads(aggregate.stdout)
            if record["run_id"] != run_id:
                reasons.append("run_id_mismatch")
        else:
            record = unaggregated_record(cell, run_id, facts)
            if not timed_out:
                reasons.append("aggregation_failed")
                sys.stderr.write(aggregate.stderr)
        if reasons:
            stderr = (run_dir / "stderr.txt").read_text(encoding="utf-8", errors="replace")
            sys.stderr.write(stderr[-2000:])
        # The evidence stays while a participant that could not be removed may still use it.
        if not self.keep and not remaining:
            remove_run_directory(run_dir)

        outcome = record["outcome"]
        outcome["reasons"] = reasons + outcome["reasons"]
        outcome["valid"] = not outcome["reasons"]
        if stray:
            outcome["warnings"].append("stray_participants")
        if ring_entries:
            outcome["warnings"].append("ring_dir_not_empty")
        record = {
            "schema": SCHEMA,
            "run_id": run_id,
            "pair_id": pair_id,
            "arm": arm,
            "order": order,
            "aa": self.aa,
            "commit": self.commits[arm],
            "note": self.note,
            "host": self.host,
            "build": record["build"],
            "cell": {"id": cell["id"], **record["cell"], "calibration": cell["calibration"]},
            "counts": record["counts"],
            "throughput": record["throughput"],
            "latency": record["latency"],
            "cpu": record["cpu"],
            "lifecycle": record["lifecycle"],
            "outcome": {"exit_code": exit_code, "timed_out": timed_out, **outcome},
            "runner": {
                "started_unix": started, "wall_s": wall, "timeout_s": timeout,
                "ring_dir_sintra_entries": ring_entries,
            },
        }
        self.output.write(json.dumps(record) + "\n")
        self.output.flush()
        print(f"{cell['id']} arm={arm} run={run_id} valid={outcome['valid']} "
              f"reasons={outcome['reasons']} warnings={outcome['warnings']}", flush=True)
        if remaining:
            fail(f"cleanup incomplete: participants {remaining} of run {run_id} are still "
                 "running. The session stops here, so that nothing is measured beside them; "
                 f"the run directory {run_dir} is kept.")
        return record


def run_pairs(session, cells, pairs):
    """A runs first in the even pairs of a cell and B in the odd ones."""
    records = []
    for cell in cells:
        for pair in range(pairs):
            pair_id = f"{session.id}:{cell['id']}:{pair}"
            for order, arm in enumerate("AB" if pair % 2 == 0 else "BA"):
                records.append(session.sample(arm, cell, pair_id, order))
    return records


def calibrate(session, cells, workloads, path):
    """Rescales each windowed count to the target interval and derives the open-loop rates."""
    for cell in cells:
        samples = [session.sample("A", cell) for _ in range(CALIBRATION_RUNS)]
        valid = [sample for sample in samples if is_usable(sample)]
        if len(valid) < CALIBRATION_RUNS:
            print(f"{cell['id']}: not calibrated, {len(valid)} of {CALIBRATION_RUNS} runs usable")
            continue
        rate = statistics.median(sample["throughput"]["publication_msg_s"] for sample in valid)
        interval = statistics.median(sample["throughput"]["interval_s"] for sample in valid)
        entry = cell["entry"]
        # A count is an integer, also below ten, where two significant digits leave a fraction.
        entry["messages"] = max(1, round(round_significant(
            entry["messages"] * workloads["target_interval_s"] / interval)))
        entry["open_loop_rates"] = [
            round_significant(rate * fraction)
            for fraction in workloads["open_loop"]["rate_fractions"]]
        entry["calibration"] = {
            "host_label": session.host["label"],
            "commit": session.commits["A"],
            "date": datetime.date.today().isoformat(),
        }
        print(f"{cell['id']}: messages={entry['messages']} "
              f"open_loop_rates={entry['open_loop_rates']}")
    write_workloads(workloads, path)
    print(f"calibrated workloads written to {path}")


# --- Summary -----------------------------------------------------------------

def load_records(path):
    if path is None or not path.is_file():
        return []
    with path.open(encoding="utf-8") as lines:
        return [json.loads(line) for line in lines if line.strip()]


def is_usable(record):
    """Invalid and validation-only records stay out of comparisons and summaries."""
    return record["outcome"]["valid"] and not record["cell"]["validation_only"]


def cpu_value(record, read):
    """A value derived from the record's CPU object, or None when a process CPU clock did
    not advance over the interval: the sample's CPU metrics are then unavailable."""
    if "cpu_not_advancing" in record["outcome"]["warnings"]:
        return None
    return read(record["cpu"])


def comparison_key(record):
    """The effective parameters of a sample: its cell (ID, planned message and warm-up counts,
    offered rate, calibration metadata), build facts, host label and C.

    The commit and the observed counts are not part of it.
    """
    cell = {key: value for key, value in record["cell"].items() if key != "framed_bytes"}
    host = record["host"]
    return json.dumps(
        [cell, record["build"], host["label"], host["cpus_effective"]], sort_keys=True)


def comparable_pairs(records):
    """The (A, B) samples of every pair in which both are usable and have equal keys."""
    arms_by_pair = {}
    for record in records:
        if record["pair_id"] is not None:
            arms_by_pair.setdefault(record["pair_id"], {})[record["arm"]] = record
    pairs = []
    for arms in arms_by_pair.values():
        a, b = arms.get("A"), arms.get("B")
        if (a and b and is_usable(a) and is_usable(b)
                and comparison_key(a) == comparison_key(b)):
            pairs.append((a, b))
    return pairs


HEADLINE_LATENCIES = {
    "windowed": ["slowest_from_send"],
    "open_loop": ["slowest_from_intended", "lateness"],
}


def headline_metrics(record):
    """(label, always a difference, value) per headline metric of the record's mode.

    CPU and the lost fraction are routinely zero or unavailable, so they are always shown as
    per-arm values with their difference. A value is None when it is unavailable.
    """
    latency = record["latency"]
    if record["cell"]["mode"] == "windowed":
        throughput = record["throughput"]
        metrics = [
            ("publication msg/s", False, throughput["publication_msg_s"]),
            ("delivery msg/s", False, throughput["delivery_msg_s"])]
        metrics += [
            (f"CPU us/msg {role}", True,
             cpu_value(record, lambda cpu, role=role: cpu[role]["us_per_message"]))
            for role in record["cpu"]]
        return metrics + [
            ("slowest_from_send p50 us", False, latency["slowest_from_send"]["p50_us"]),
            ("slowest_from_send p99 us", False, latency["slowest_from_send"]["p99_us"])]
    return [
        ("slowest_from_intended p50 us", False, latency["slowest_from_intended"]["p50_us"]),
        ("slowest_from_intended p99 us", False, latency["slowest_from_intended"]["p99_us"]),
        ("lost fraction", True, lost_fraction(record)),
        ("lateness p99 us", False, latency["lateness"]["p99_us"])]


def lost_fraction(record):
    counts = record["counts"]
    return counts["lost"] / counts["deliveries_expected"]


def metric_values(pairs):
    """Per headline metric label: (always a difference, [(value in A, value in B) per pair])."""
    values = {}
    for a, b in pairs:
        for (label, always_difference, in_a), (_, _, in_b) in zip(
                headline_metrics(a), headline_metrics(b)):
            values.setdefault(label, (always_difference, []))[1].append((in_a, in_b))
    return values


def compare(value_pairs, as_difference):
    """The within-pair comparisons, B-A or B/A, of the pairs that have a value in both arms,
    and the number of pairs that lack one."""
    present = [(a, b) for a, b in value_pairs if a is not None and b is not None]
    compared = [b - a if as_difference else b / a for a, b in present]
    return present, compared, len(value_pairs) - len(present)


def value_range(values):
    return f"[{min(values):.4g}, {max(values):.4g}]"


def coverage(compared, unavailable):
    text = f"over {len(compared)} pairs"
    return f"{text}, {unavailable} unavailable" if unavailable else text


def print_summary(records, aa_records, skipped):
    """Prints the comparison of the session's pairs, cell by cell.

    Beside each comparison stands the range observed over the comparable A/A pairs in
    `aa_records` that have the same effective parameters. Nothing is flagged.
    """
    print("\n== Summary: median [min, max] of the within-pair comparisons ==")
    print("B/A is a ratio. CPU, the lost fraction, and any metric with a zero among its")
    print("observations or its A/A observations are shown as per-arm medians with B-A instead.")
    pairs_by_cell = {}
    for a, b in comparable_pairs(records):
        pairs_by_cell.setdefault(a["cell"]["id"], []).append((a, b))
    aa_pairs_by_key = {}
    for a, b in comparable_pairs(aa_records):
        aa_pairs_by_key.setdefault(comparison_key(a), []).append((a, b))
    samples_by_cell = {}
    for record in records:
        if record["pair_id"] is not None:
            samples_by_cell.setdefault(record["cell"]["id"], []).append(record)

    for cell_id, samples in samples_by_cell.items():
        pairs = pairs_by_cell.get(cell_id, [])
        attempted = len({sample["pair_id"] for sample in samples})
        invalid = {arm: sum(1 for sample in samples
                            if sample["arm"] == arm and not sample["outcome"]["valid"])
                   for arm in "AB"}
        print(f"{cell_id}: {len(pairs)} comparable pairs of {attempted}; "
              f"invalid samples: A {invalid['A']}, B {invalid['B']}")
        if not pairs:
            continue
        for name in HEADLINE_LATENCIES[pairs[0][0]["cell"]["mode"]]:
            least = min((sample["latency"][name] for pair in pairs for sample in pair),
                        key=lambda latency: latency["n_messages"])
            print(f"  sample support of {name}: at least n_obs {least['n_obs']}, "
                  f"n_messages {least['n_messages']}")
        aa_values = metric_values(aa_pairs_by_key.get(comparison_key(pairs[0][0]), []))
        for label, (always_difference, value_pairs) in metric_values(pairs).items():
            aa_value_pairs = aa_values.get(label, (always_difference, []))[1]
            # One representation serves the comparison and its A/A range, and it is chosen
            # from both: a zero observation in either rules the ratio out. No observation is
            # dropped; one without a value is counted as unavailable.
            as_difference = always_difference or any(
                value == 0 for pair in value_pairs + aa_value_pairs for value in pair)
            present, compared, unavailable = compare(value_pairs, as_difference)
            if not compared:
                text = f"unavailable in all {unavailable} pairs"
            elif as_difference:
                text = (f"A {statistics.median(a for a, _ in present):.4g}  "
                        f"B {statistics.median(b for _, b in present):.4g}  "
                        f"B-A {statistics.median(compared):.4g} {value_range(compared)} "
                        f"{coverage(compared, unavailable)}")
            else:
                text = (f"B/A {statistics.median(compared):.4g} {value_range(compared)} "
                        f"{coverage(compared, unavailable)}")
            _, aa_compared, aa_unavailable = compare(aa_value_pairs, as_difference)
            if not aa_value_pairs:
                aa_text = "unavailable (no matching A/A pairs)"
            elif not aa_compared:
                aa_text = f"unavailable in all {aa_unavailable} pairs"
            else:
                aa_text = f"{value_range(aa_compared)} {coverage(aa_compared, aa_unavailable)}"
            print(f"  {label:30} {text}; observed A/A range {aa_text}")
    if skipped:
        print("skipped, no stored open-loop rate: " + ", ".join(cell["id"] for cell in skipped))


# --- Axis tables ---------------------------------------------------------------

def show(value):
    if value is None:
        return "-"
    if not isinstance(value, float):
        return str(value)
    return f"{value:.0f}" if abs(value) >= 10000 else f"{value:.4g}"


def print_table(title, columns, rows):
    print(f"\n{title}")
    lines = [columns] + [[show(value) for value in row] for row in rows]
    widths = [max(len(line[index]) for line in lines) for index in range(len(columns))]
    for line in lines:
        print("  " + "  ".join(text.rjust(width) for text, width in zip(line, widths)))


def cpu_us_per_megabyte(sample):
    megabytes = sample["counts"]["published"] * sample["cell"]["payload_bytes"] / 1.0e6
    return cpu_value(sample, lambda cpu: cpu["total"]["cpu_s"] * 1.0e6 / megabytes)


def print_axis_tables(records):
    """Hotspot readings for arm A: the median over the usable samples of each cell.

    A sample whose process CPU did not advance contributes no CPU value.
    """
    samples_by_cell = {}
    for record in records:
        if record["arm"] == "A" and is_usable(record):
            samples_by_cell.setdefault(record["cell"]["id"], []).append(record)
    if not samples_by_cell:
        return
    cells = {cell_id: samples[0]["cell"] for cell_id, samples in samples_by_cell.items()}

    def median(cell_id, read):
        values = [read(sample) for sample in samples_by_cell[cell_id]]
        values = [value for value in values if value is not None]
        return statistics.median(values) if values else None

    def cpu_median(cell_id, read):
        return median(cell_id, lambda sample: cpu_value(sample, read))

    def cell_ids(mode, **parameters):
        return [cell_id for cell_id, cell in cells.items() if cell["mode"] == mode
                and all(cell[key] == value for key, value in parameters.items())]

    def axis(mode, *keys):
        return sorted({tuple(cells[cell_id][key] for key in keys) for cell_id in cell_ids(mode)})

    print("\n== Axis tables: arm A, median over usable samples ==")
    for origin, payload in axis("windowed", "origin", "payload_bytes"):
        ids = cell_ids("windowed", origin=origin, payload_bytes=payload)
        roles = list(samples_by_cell[ids[0]][0]["cpu"])
        rows = [[cells[cell_id]["receivers"]]
                + [cpu_median(cell_id, lambda cpu, role=role: cpu[role]["us_per_message"])
                   for role in roles]
                + [cpu_median(cell_id, lambda cpu: cpu["receivers"]["us_per_delivery"])]
                for cell_id in sorted(ids, key=lambda cell_id: cells[cell_id]["receivers"])]
        print_table(
            f"CPU us per message by role against R: origin {origin}, payload {payload} B",
            ["R"] + roles + ["receivers us/delivery"], rows)

    for (origin,) in axis("windowed", "origin"):
        receivers = [r for (r,) in axis("windowed", "receivers")]
        rows = []
        for (payload,) in axis("windowed", "payload_bytes"):
            row = [payload]
            for count in receivers:
                ids = cell_ids("windowed", origin=origin, payload_bytes=payload, receivers=count)
                row.append(median(ids[0], cpu_us_per_megabyte) if ids else None)
            rows.append(row)
        print_table(
            f"Total CPU us per application MB against payload: origin {origin}",
            ["payload B"] + [f"R={count}" for count in receivers], rows)

    for origin, count, payload in axis("open_loop", "origin", "receivers", "payload_bytes"):
        ids = cell_ids("open_loop", origin=origin, receivers=count, payload_bytes=payload)
        rows = [[cells[cell_id]["offered_rate"],
                 median(cell_id, lambda s: s["latency"]["slowest_from_intended"]["p50_us"]),
                 median(cell_id, lambda s: s["latency"]["slowest_from_intended"]["p99_us"]),
                 median(cell_id, lost_fraction),
                 median(cell_id, lambda s: s["latency"]["lateness"]["p99_us"])]
                for cell_id in sorted(ids, key=lambda cell_id: cells[cell_id]["offered_rate"])]
        print_table(
            f"Open-loop latency and loss against offered rate: origin {origin}, R={count}, "
            f"payload {payload} B",
            ["offered msg/s", "slowest p50 us", "slowest p99 us", "lost fraction",
             "lateness p99 us"], rows)

    def share(role):
        def read(cpu):
            if role not in cpu:
                return None
            return 100.0 * cpu[role]["cpu_s"] / cpu["total"]["cpu_s"]
        return read

    print_table(
        "Share of total CPU by role, percent", ["cell"] + ROLES,
        [[cell_id] + [cpu_median(cell_id, share(role)) for role in ROLES] for cell_id in cells])


# --- Entry point ---------------------------------------------------------------

def positive_int(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--a", required=True, type=Path, help="build directory of arm A")
    parser.add_argument("--a-commit", required=True, help="commit that arm A was built from")
    parser.add_argument("--b", type=Path, help="build directory of arm B; absent for A/A")
    parser.add_argument("--b-commit", help="commit that arm B was built from")
    parser.add_argument("--output", required=True, type=Path, help="JSONL file to append to")
    parser.add_argument(
        "--workloads", type=Path, default=Path(__file__).with_name("workloads.json"))
    parser.add_argument("--pairs", type=positive_int, default=5)
    parser.add_argument("--select", action="append", metavar="GLOB",
                        help="cell-id glob; repeat it to select more cells")
    parser.add_argument("--note", default="")
    parser.add_argument("--cpus", type=positive_int,
                        help="effective CPU count C of this session")
    parser.add_argument("--aa", type=Path,
                        help="JSONL file with more A/A records for the observed A/A range")
    parser.add_argument("--calibrate", type=Path, metavar="NEW_WORKLOADS",
                        help="calibrate the selected windowed cells into this workloads file")
    parser.add_argument("--keep", action="store_true", help="keep the run directories")
    args = parser.parse_args()
    if args.b is not None and args.b_commit is None:
        parser.error("--b needs --b-commit")
    return args


def interrupt(signum, frame):
    raise KeyboardInterrupt


def main():
    # A termination request unwinds like Ctrl+C, so that the running sample is cleaned up.
    for name in ("SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), interrupt)

    args = parse_arguments()
    workloads = json.loads(args.workloads.read_text(encoding="utf-8"))
    validate_workloads(workloads)
    cells = select_cells(expand_cells(workloads), args.select)
    if args.calibrate:
        cells = [cell for cell in cells if cell["mode"] == "windowed"]
    # An open-loop cell has no message count until calibration has stored its rate.
    skipped = [cell for cell in cells if cell["messages"] is None]
    cells = [cell for cell in cells if cell["messages"] is not None]
    if skipped:
        print("skipped, no stored open-loop rate: " + ", ".join(cell["id"] for cell in skipped))
    if not cells:
        fail("no cell to run")

    earlier_aa = [record for record in load_records(args.output) + load_records(args.aa)
                  if record["aa"]]
    session = Session(args, cells)
    try:
        if args.calibrate:
            calibrate(session, cells, workloads, args.calibrate)
            return
        records = run_pairs(session, cells, args.pairs)
    except KeyboardInterrupt:
        fail("interrupted. The running sample was terminated and has no record.")
    finally:
        session.close()
    # The A/A records of the output file include this session's own, if it is an A/A session.
    print_summary(records, earlier_aa + (records if session.aa else []), skipped)
    print_axis_tables(records)


if __name__ == "__main__":
    main()
