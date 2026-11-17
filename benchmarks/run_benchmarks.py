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
import shutil
import signal
import statistics
import subprocess
import sys
import time

SCHEMA = "sintra-bench/1"
EXECUTABLES = {"broadcast": "sintra_broadcast_benchmark", "rpc": "sintra_rpc_benchmark",
               "swarm": "sintra_swarm_benchmark"}
CELL_PREFIXES = {"broadcast": "w1", "rpc": "w2", "swarm": "w3"}
TIMEOUT_BASE_S = 60         # a sample's timeout is this plus a multiple of its planned duration:
TIMEOUT_FACTOR = 3          # three times it, or, for a windowed broadcast cell that was never
UNCALIBRATED_TIMEOUT_FACTOR = 30    # calibrated, whose count is only an estimate, thirty times it
CALIBRATION_TIMEOUT_S = 900 # under --calibrate: an estimate may be far off on a slow host
REPORT_WAIT_S = 10          # for the reports of every participant, after the coordinator's exit
EXIT_WAIT_S = 10            # the cleanup's deadline: two consecutive empty looks must end by it
AGGREGATE_TIMEOUT_S = 60    # for --aggregate, which only reads the reports of one sample
BUILD_FACTS_TIMEOUT_S = 60  # for --build-facts, which only prints constants
OBSERVE_INTERVAL_S = 0.25   # between looks at the participants while the coordinator runs
CALIBRATION_RUNS = 3
ROLES = ["coordinator", "coordinator_sender", "sender", "receivers"]
HAS_PROCFS = Path("/proc/self/cmdline").exists()

# The build facts that both arms of a comparison must share, and the library constants in
# which they may differ: a change of the library is what a comparison is there to measure.
CONFIGURATION_FACTS = [
    "compiler", "config", "ndebug", "optimized", "test_hooks", "separate_compilation"]
LIBRARY_FACTS = [
    "slow_reader_eviction", "eviction_spin_budget_us", "eviction_spin_threshold",
    "eviction_lag_rings", "message_ring_size", "ring_abi_version"]

# The keys of a record's cell that every workload writes (plan section 7), besides its
# origin or route, and besides the ID and the calibration metadata that the runner adds.
CELL_KEYS = [
    "workload", "mode", "receivers", "payload_bytes", "framed_bytes", "gap_us", "window",
    "n_leaves", "offered_rate", "planned", "warmup", "burst", "handler_delay_us",
    "drain_timeout_s", "validation_only"]


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
            if entry["workload"] == "swarm":
                problems.append(f"{where}: swarm cells come from the top-level swarm key")
                continue
            if entry["workload"] not in EXECUTABLES:
                problems.append(f"{where}: unknown workload {entry['workload']!r}")
            if entry["workload"] == "rpc":
                problems += rpc_entry_problems(entry, where)
                continue
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
    """Every defined cell: per broadcast entry, a windowed cell and one open-loop cell per
    rate fraction; per request/response entry, one cell."""
    open_loop = workloads["open_loop"]
    cells = []
    for entry in workloads["cells"]:
        if entry["workload"] == "rpc":
            cells.append(expand_rpc_cell(entry))
            continue
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


# --- Request/response cells (W2) ------------------------------------------------

RPC_WARMUP_CALLS = 1000
# The roles of a route's processes, by branch index (role_name in rpc_benchmark.cpp).
RPC_ROUTE_ROLES = {
    "leaf_to_coordinator": ["coordinator_callee", "caller"],
    "leaf_to_leaf": ["coordinator", "callee", "caller"],
}
RPC_VALIDATION_KEYS = ("handler_delay_us", "wrong_echo_every", "throw_every")
RPC_HEADLINE_LATENCIES = {
    "closed_loop": ["rtt"],
    "windowed": ["issue_to_observation"],
}
# What the timeout of a sample plans for one call. A closed-loop call is planned as its gap
# plus a round trip in which every hop can wait for a reader's next poll. A windowed call
# shares such a round trip with the rest of its window.
RPC_PLANNED_ROUND_TRIP_US = 5000
RPC_PLANNED_WINDOWED_CALL_US = 250


def rpc_entry_problems(entry, where):
    """The problems of one request/response entry of a workloads file."""
    problems = []
    if entry["route"] not in RPC_ROUTE_ROLES:
        problems.append(f"{where}: route must be leaf_to_coordinator or leaf_to_leaf")
    if entry["mode"] not in RPC_HEADLINE_LATENCIES:
        problems.append(f"{where}: mode must be closed_loop or windowed")
    if not is_count(entry["calls"]):
        problems.append(f"{where}: calls must be a positive integer")
    # The gap belongs to closed-loop mode and the window to windowed mode. The key of the
    # other mode would have no effect, so it is an error and is not dropped.
    if entry["mode"] == "closed_loop":
        if not is_count(entry["gap_us"], least=0):
            problems.append(f"{where}: gap_us must be a non-negative integer")
        if entry.get("window") is not None:
            problems.append(f"{where}: window belongs to windowed mode")
    if entry["mode"] == "windowed":
        if not is_count(entry["window"]):
            problems.append(f"{where}: window must be a positive integer")
        if entry.get("gap_us") is not None:
            problems.append(f"{where}: gap_us belongs to closed_loop mode")
    for key in RPC_VALIDATION_KEYS:
        if entry.get(key) is not None and not is_count(entry[key], least=0):
            problems.append(f"{where}: {key} must be a non-negative integer")
    # The delay runs on the reader thread that dispatches the call, and a validation run does
    # not slow down a reader of the coordinator.
    if entry.get("handler_delay_us") and entry["route"] == "leaf_to_coordinator":
        problems.append(f"{where}: handler_delay_us needs the leaf_to_leaf route")
    return problems


def rpc_planned_duration_s(entry):
    """The planned duration of a request/response cell, which its timeout follows. The counts
    are fixed, so it does not come from a calibration."""
    if entry["mode"] == "closed_loop":
        return entry["calls"] * (entry["gap_us"] + RPC_PLANNED_ROUND_TRIP_US) / 1.0e6
    return entry["calls"] * RPC_PLANNED_WINDOWED_CALL_US / 1.0e6


def expand_rpc_cell(entry):
    """The one cell of a request/response entry."""
    closed_loop = entry["mode"] == "closed_loop"
    name = f"g{entry['gap_us']}" if closed_loop else f"w{entry['window']}"
    cell = {
        "workload": "rpc",
        "id": f"{CELL_PREFIXES['rpc']}.{entry['mode']}.{entry['route']}.{name}",
        "mode": entry["mode"],
        "route": entry["route"],
        "gap_us": entry["gap_us"] if closed_loop else None,
        "window": None if closed_loop else entry["window"],
        "calls": entry["calls"],
        "duration_s": rpc_planned_duration_s(entry),
        "calibration": None,
        "default": True,
    }
    cell.update((key, entry.get(key)) for key in RPC_VALIDATION_KEYS)
    return cell


def rpc_command(executable, cell, run_id):
    command = [
        str(executable), "--mode", cell["mode"], "--route", cell["route"],
        "--calls", str(cell["calls"]), "--warmup", str(RPC_WARMUP_CALLS),
        "--run-id", run_id]
    for flag, key in (("--gap-us", "gap_us"), ("--window", "window"),
                      ("--handler-delay-us", "handler_delay_us"),
                      ("--wrong-echo-every", "wrong_echo_every"),
                      ("--throw-every", "throw_every")):
        if cell[key] is not None:
            command += [flag, str(cell[key])]
    return command


def rpc_unaggregated_record(cell, run_id, facts):
    """As unaggregated_record, for a request/response cell."""
    return fallback_record(
        run_id, facts, workload="rpc", mode=cell["mode"], route=cell["route"],
        gap_us=cell["gap_us"], window=cell["window"],
        n_leaves=2 if cell["route"] == "leaf_to_leaf" else 1,
        planned=cell["calls"], warmup=RPC_WARMUP_CALLS,
        handler_delay_us=cell["handler_delay_us"] or 0,
        validation_only=any(cell[key] for key in RPC_VALIDATION_KEYS))


# --- Swarm cells (W3) ---------------------------------------------------------

SWARM_MAX_LEAVES = 125      # the automatic process indexes that a coordinator can hand out


def validate_swarm(workloads):
    """Rejects swarm sizes that a coordinator cannot start, and malformed swarm options."""
    swarm = workloads.get("swarm")
    if swarm is None:
        return
    problems = []
    try:
        sizes = swarm["leaves"] + [swarm["oversubscribed_least"]]
        if not all(is_count(size) and size <= SWARM_MAX_LEAVES for size in sizes):
            problems.append("swarm.leaves and swarm.oversubscribed_least must be integers "
                            f"from 1 to {SWARM_MAX_LEAVES}")
        elif len(set(swarm["leaves"])) != len(swarm["leaves"]):
            problems.append("swarm.leaves repeats a size")
        if not is_count(swarm["oversubscribed_per_cpu"]):
            problems.append("swarm.oversubscribed_per_cpu must be a positive integer")
        if not isinstance(swarm.get("fail_init", False), bool):
            problems.append("swarm.fail_init must be true or false")
        if not is_count(swarm.get("leaf_exit_delay_ms", 0), least=0):
            problems.append("swarm.leaf_exit_delay_ms must be a non-negative integer")
    except (KeyError, TypeError) as error:
        problems.append(f"malformed swarm entry or missing key: {error!r}")
    if problems:
        fail("invalid workloads file:\n  " + "\n  ".join(problems))


def expand_swarm_cells(workloads):
    """One lifecycle cell per swarm size, and the oversubscribed cell."""
    swarm = workloads.get("swarm")
    if swarm is None:
        return []
    # A swarm sample has no planned duration: its timeout is the base timeout.
    base = {
        "workload": "swarm", "mode": "lifecycle", "default": True, "calibration": None,
        "duration_s": 0, "not_oversubscribed": False,
        "fail_init": swarm.get("fail_init", False),
        "leaf_exit_delay_ms": swarm.get("leaf_exit_delay_ms", 0),
    }
    prefix = CELL_PREFIXES["swarm"]
    cells = [dict(base, id=f"{prefix}.lifecycle.n{leaves}", leaves=leaves)
             for leaves in swarm["leaves"]]
    # The size of the oversubscribed cell follows from C, which the session establishes.
    return cells + [dict(base, id=f"{prefix}.lifecycle.n_over", leaves=None)]


def size_swarm_cells(cells, swarm, cpus):
    """Gives every swarm cell the session's C, and the oversubscribed cell its size
    N_over = min(125, max(least, per_cpu * C)).

    Returns the cells that can run and those that cannot: without C, the oversubscribed
    cell has no size.
    """
    def is_unsized(cell):
        return cell["workload"] == "swarm" and cell["leaves"] is None

    for cell in cells:
        if cell["workload"] == "swarm":
            cell["cpus"] = cpus
        if is_unsized(cell) and cpus != 0:
            cell["leaves"] = min(SWARM_MAX_LEAVES, max(
                swarm["oversubscribed_least"], swarm["oversubscribed_per_cpu"] * cpus))
            cell["not_oversubscribed"] = cell["leaves"] + 1 <= cpus
    return ([cell for cell in cells if not is_unsized(cell)],
            [cell for cell in cells if is_unsized(cell)])


def report_unsized(cells):
    if cells:
        print("skipped, C is unknown (hardware_concurrency() is 0 and --cpus is absent): "
              + ", ".join(cell["id"] for cell in cells))


def swarm_command(executable, cell, run_id):
    command = [str(executable), "--leaves", str(cell["leaves"]), "--cpus", str(cell["cpus"]),
               "--run-id", run_id]
    if cell["fail_init"]:
        command.append("--fail-init")
    if cell["leaf_exit_delay_ms"]:
        command += ["--leaf-exit-delay-ms", str(cell["leaf_exit_delay_ms"])]
    return command


def swarm_unaggregated_record(cell, run_id, facts):
    """As unaggregated_record, for a swarm cell."""
    return fallback_record(
        run_id, facts, workload="swarm", mode="lifecycle", origin=None,
        n_leaves=cell["leaves"],
        validation_only=bool(cell["fail_init"] or cell["leaf_exit_delay_ms"]))


# --- Ownership of a sample's processes ----------------------------------------

interrupt_requested = False


def request_interrupt(signum, frame):
    """The handler of SIGINT, SIGTERM, SIGHUP, SIGQUIT and Ctrl+Break. It only records the
    request.

    Nothing is raised asynchronously. The runner acts on the request where it polls for it,
    and a cleanup that is under way runs to its end whatever signals arrive meanwhile.
    """
    global interrupt_requested
    interrupt_requested = True


def names_run(arguments, run_id):
    """Whether a command line carries `--run-id <run ID>`, as the coordinator's and every
    leaf's does."""
    return ("--run-id", run_id) in zip(arguments, arguments[1:])


def command_line(pid):
    """The arguments of the process that has this PID now, from procfs; empty when it is gone.

    Only a process that has disappeared counts as gone. Any other failure to read, such as a
    missing permission, raises.
    """
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except (FileNotFoundError, ProcessLookupError):
        return []
    return [argument.decode(errors="replace") for argument in raw.split(b"\0")]


def run_processes(run_id, timeout):
    """{PID: command line} of every process whose command line names this run, and
    {PID: error} of each process whose command line could not be read.

    A child that the coordinator has forked, but that has not started the executable yet,
    still shows the coordinator's command line. A scan therefore finds the processes of a run
    without their cooperation; a file that a process writes once it runs does not.

    With procfs, only the processes of the runner's user are read: every participant runs as
    that user, and procfs can deny the command line of another user's process (hidepid). The
    owner of /proc/<pid> is the process's effective user. A failure to read one process does
    not end the scan. With `ps`, a failed `ps` raises, and so does one that has not finished
    within `timeout` seconds (subprocess.TimeoutExpired).
    """
    unread = {}
    if HAS_PROCFS:
        lines = {}
        user = os.geteuid()
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            pid = int(entry)
            try:
                if os.stat(f"/proc/{pid}").st_uid == user:
                    lines[pid] = command_line(pid)
            except (FileNotFoundError, ProcessLookupError):
                pass    # it has gone
            except OSError as error:
                unread[pid] = error
    else:
        listing = subprocess.run(
            ["ps", "-ww", "-A", "-o", "pid=", "-o", "command="],
            capture_output=True, text=True, errors="replace", check=True, timeout=timeout)
        fields = [line.split() for line in listing.stdout.splitlines() if line.strip()]
        lines = {int(pid): arguments for pid, *arguments in fields}
    found = {pid: " ".join(arguments).strip() for pid, arguments in lines.items()
             if names_run(arguments, run_id)}
    return found, unread


class Participants:
    """The processes of one sample: the coordinator that the runner launches, and its leaves.

    The runner signals only what it holds by more than a number: the coordinator, which is
    its own child, and on Windows each descendant, through a retained handle. It never
    signals a bare PID, not even one that it has just checked: between the check and the
    signal, the PID can pass to another process. A subclass provides the platform's part.
    """

    def __init__(self, run_id):
        self.run_id = run_id
        self.process = None

    def launch(self, command, run_dir):
        """Starts the coordinator and takes ownership of its swarm."""
        with open(run_dir / "stdout.txt", "wb") as out, open(run_dir / "stderr.txt", "wb") as err:
            self.process = subprocess.Popen(
                command, stdout=out, stderr=err,
                env=dict(os.environ, SINTRA_BENCHMARK_DIR=str(run_dir)),
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                start_new_session=os.name != "nt")
        self.adopt()

    def adopt(self):
        """Takes whatever the platform needs to own the launched coordinator's swarm."""

    def observe(self):
        """Called repeatedly while the coordinator runs."""

    def kill_owned(self):
        """Terminates everything that is owned; returns a description of each failure."""
        raise NotImplementedError

    def alive(self, timeout):
        """Looks afresh and describes each process of the run that is alive, the coordinator
        until it is reaped, and whatever keeps the look from being conclusive. A `ps` call
        that has not finished within `timeout` seconds raises."""
        raise NotImplementedError

    def close(self):
        pass

    def wait_for_coordinator(self, timeout):
        """Waits for the coordinator; returns "exited", "timeout" or "interrupted"."""
        deadline = time.monotonic() + timeout
        while True:
            if interrupt_requested:
                return "interrupted"
            self.observe()
            try:
                self.process.wait(
                    timeout=max(0, min(OBSERVE_INTERVAL_S, deadline - time.monotonic())))
                return "exited"
            except subprocess.TimeoutExpired:
                if time.monotonic() >= deadline:
                    return "timeout"

    def clean_up(self):
        """Terminates what is owned, reaps the coordinator and looks until two consecutive
        looks find no process of the run.

        Both looks must end before the deadline, EXIT_WAIT_S after the cleanup starts. The
        deadline does not bound the cleanup's own duration: a step under way at the deadline
        runs to its end, except a `ps` call, which is cut off there (an inconclusive look).

        It does not raise, and a step that fails does not keep the others from running. It
        returns a description of whatever is left or could not be established; the cleanup
        is complete when that is empty.
        """
        if self.process is None:
            return []   # the launch failed, so there is nothing to clean up
        deadline = time.monotonic() + EXIT_WAIT_S
        empty_looks = 0
        while True:
            remaining = []
            try:
                remaining += self.kill_owned()
            except Exception as error:
                remaining.append(f"the termination failed: {error!r}")
            try:
                remaining += self.alive(max(0.0, deadline - time.monotonic()))
            except Exception as error:
                remaining.append(f"the processes of the run could not be listed: {error!r}")
            # On POSIX a process is invisible to a look for a moment while it starts its
            # executable, so the cleanup is complete only after two consecutive empty looks,
            # both of which end before the deadline.
            empty_looks = 0 if remaining else empty_looks + 1
            if time.monotonic() >= deadline:
                return remaining or [
                    "no two consecutive empty looks ended before the deadline"]
            if empty_looks == 2:
                return []
            time.sleep(0.05)


class Windows_participants(Participants):
    """Owns the coordinator's process tree through retained native handles.

    The tree starts from the handle of the coordinator that the runner launched and adds the
    handle of each descendant while it is alive. Termination addresses those handles, so no
    PID is signalled.
    """

    def __init__(self, run_id, process_tree):
        super().__init__(run_id)
        self.process_tree = process_tree
        self.tree = None

    def adopt(self):
        self.tree = self.process_tree(self.process)

    def observe(self):
        self.tree.refresh()

    def kill_owned(self):
        errors = []
        if self.tree is not None:
            try:
                self.tree.terminate()
            except OSError as error:
                errors.append(f"process tree: {error}")
        try:
            self.process.kill()     # through the handle that Popen holds
        except OSError as error:
            errors.append(f"pid {self.process.pid}, the coordinator: {error}")
        return errors

    def alive(self, timeout):
        alive = []      # the timeout is for `ps`; the retained process tree does not use it
        if self.process.poll() is None:
            alive.append(f"pid {self.process.pid}: the coordinator")
        if self.tree is None:
            return alive + ["descendants unknown: their process tree could not be established"]
        alive += [f"pid {pid}" for pid in self.tree.live_descendants()]
        if self.tree.enumeration_error is not None:
            alive.append("descendants unknown: the process table could not be read "
                         f"({self.tree.enumeration_error})")
        return alive

    def close(self):
        if self.tree is not None:
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

    class Retained_process_tree(module.Windows_process_tree):
        """terminate() and live_descendants() begin with a refresh. A process table that
        cannot be read must not keep them from the handles that are already retained, so the
        failure is recorded here, and the caller reports it as unresolved ownership."""

        enumeration_error = None

        def refresh(self):
            try:
                super().refresh()
                self.enumeration_error = None
            except OSError as error:
                self.enumeration_error = error

    return Retained_process_tree


class Posix_participants(Participants):
    """Owns the coordinator as the runner's child, and signals only the coordinator's process
    group.

    That group holds the coordinator and those of its children that have not left it for a
    session of their own yet; such a child receives the group's signal too. A leaf that has
    entered its own session is never signalled, and neither is any PID that a scan finds:
    the runner knows those only by their PIDs. Once the coordinator is gone, a leaf exits by
    itself: every spawned Sintra process holds a lifeline to the coordinator and hard-exits
    about 100 ms after it closes (docs/architecture.md, "Lifeline ownership"). The runner
    waits for that and looks at the process table for processes that name this run. A leaf
    that cannot run, such as a stopped one, cannot exit that way: the cleanup reports it as
    left, and the operator ends it (owner decision, plan section 14).
    """

    def kill_owned(self):
        try:
            # The coordinator leads the process group that the runner started it in. While
            # it is an unreaped child of the runner, neither its PID nor that group ID can
            # pass to another process, and the group holds only the coordinator and those of
            # its children that have not started their own session yet.
            if self.process.poll() is None:
                os.killpg(self.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError as error:
            return [f"pid {self.process.pid}, the coordinator: {error}"]
        return []

    def alive(self, timeout):
        found, unread = run_processes(self.run_id, timeout)
        if self.process.poll() is None:
            found.setdefault(self.process.pid, "the coordinator")
        return ([f"pid {pid}: {command}" for pid, command in sorted(found.items())]
                + [f"pid {pid}: could not be inspected ({error})"
                   for pid, error in sorted(unread.items())])


# --- Samples -----------------------------------------------------------------

def find_executable(build_dir, workload):
    name = EXECUTABLES[workload] + (".exe" if os.name == "nt" else "")
    for directory in (build_dir / "benchmarks", build_dir / "benchmarks" / "Release", build_dir):
        if (directory / name).is_file():
            # An absolute path: a bare file name would be looked up on PATH.
            return (directory / name).resolve()
    fail(f"{name} was not found under {build_dir}")


def read_build_facts(executable):
    result = subprocess.run(
        [str(executable), "--build-facts"], capture_output=True, text=True, check=True,
        timeout=BUILD_FACTS_TIMEOUT_S)
    return json.loads(result.stdout)


def configuration(build):
    """The toolchain and configuration of a build: what both arms must share. The name of
    the configuration counts in any letter case, as it does for the validity of a build."""
    return dict({key: build[key] for key in CONFIGURATION_FACTS}, config=build["config"].lower())


def library_notice(build_a, build_b):
    """Names the library constants in which the builds of the two arms differ; None when
    there is none. Such arms are compared all the same."""
    different = [f"  {key}: A {build_a.get(key)}, B {build_b.get(key)}"
                 for key in LIBRARY_FACTS if build_a.get(key) != build_b.get(key)]
    if not different:
        return None
    return "notice: the arms differ in library constants:\n" + "\n".join(different)


def warmup_messages(cell):
    return max(64, cell["messages"] // 20)


def default_burst(cell, facts):
    return max(1, facts["message_ring_size"] // 4 // (cell["payload_bytes"] + 512))


def is_validation_only(cell, facts):
    """Whether the cell's options change the effective workload: a handler delay, or, in
    windowed mode, a burst other than the default one. Open-loop publication has no bursts.
    The executable marks its records by the same rule."""
    return bool(cell["handler_delay_us"]) or (
        cell["mode"] == "windowed" and cell["burst"] not in (None, default_burst(cell, facts)))


def cell_command(executable, cell, run_id):
    if cell["workload"] == "rpc":
        return rpc_command(executable, cell, run_id)
    if cell["workload"] == "swarm":
        return swarm_command(executable, cell, run_id)
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
    """Waits up to REPORT_WAIT_S for one report per participant that recorded its PID."""
    expected = len(list(run_dir.glob("*.pid")))
    deadline = time.monotonic() + REPORT_WAIT_S
    while (len(list(run_dir.glob("*.bin"))) < expected and time.monotonic() < deadline
           and not interrupt_requested):
        time.sleep(0.05)


def remove_run_directory(run_dir):
    """Windows can keep a file of a process that has just exited busy for a moment."""
    for _ in range(100):
        shutil.rmtree(run_dir, ignore_errors=True)
        if not run_dir.exists():
            return
        time.sleep(0.05)
    print(f"warning: could not remove {run_dir}", file=sys.stderr)


def cleanup_incomplete_message(run_id, run_dir, remaining):
    return (f"cleanup incomplete. These processes of run {run_id} are still alive, or the "
            "runner could not establish that none is:\n  " + "\n  ".join(remaining) + "\n"
            "The session stops here, so that nothing is measured beside them; "
            f"the run directory {run_dir} is kept.")


def fallback_record(run_id, facts, **cell):
    """A record without metrics, as the runner writes it when `--aggregate` produced none.
    Its cell has every key that the executable would have written; a key that does not apply
    to the cell, or that only the executable knows, is null."""
    return {
        "run_id": run_id,
        "build": facts,
        "cell": {**dict.fromkeys(CELL_KEYS), **cell},
        "counts": None, "throughput": None, "latency": None, "cpu": None, "lifecycle": None,
        "outcome": {"missing_files": [], "valid": False, "reasons": [], "warnings": []},
    }


def expected_reports(cell):
    """The file names of the reports that `--aggregate` expects for the cell: one per process,
    named after its role and branch index (report_file_name in harness.h)."""
    if cell["workload"] == "rpc":
        roles = RPC_ROUTE_ROLES[cell["route"]]
    elif cell["workload"] == "swarm":
        roles = ["coordinator"] + ["leaves"] * cell["leaves"]
    elif cell["origin"] == "leaf":
        roles = ["coordinator", "sender"] + ["receivers"] * cell["receivers"]
    else:
        roles = ["coordinator_sender"] + ["receivers"] * cell["receivers"]
    return [f"{role}-{index}.bin" for index, role in enumerate(roles)]


def unaggregated_record(cell, run_id, facts, run_dir):
    """The record of a sample for which `--aggregate` produced none: the sample stalled or
    died before its processes wrote their reports, or a report could not be read.

    Its `missing_files` are the expected reports that are absent from the run directory, with
    the reason `missing_files` when there is one. A report that is present may still be one
    that could not be read.
    """
    if cell["workload"] == "rpc":
        record = rpc_unaggregated_record(cell, run_id, facts)
    elif cell["workload"] == "swarm":
        record = swarm_unaggregated_record(cell, run_id, facts)
    else:
        record = fallback_record(
            run_id, facts, workload=cell["workload"], mode=cell["mode"], origin=cell["origin"],
            receivers=cell["receivers"], payload_bytes=cell["payload_bytes"],
            n_leaves=cell["receivers"] + (cell["origin"] == "leaf"),
            offered_rate=cell["offered_rate"], planned=cell["messages"],
            warmup=warmup_messages(cell),
            burst=default_burst(cell, facts) if cell["burst"] is None else cell["burst"],
            handler_delay_us=cell["handler_delay_us"] or 0,
            validation_only=is_validation_only(cell, facts))
    missing = [name for name in expected_reports(cell) if not (run_dir / name).is_file()]
    record["outcome"]["missing_files"] = missing
    if missing:
        record["outcome"]["reasons"].append("missing_files")
    return record


class Session:
    """The binaries, the host facts and the output of one runner invocation."""

    def __init__(self, args, cells):
        # Without --b, the A binaries run as both arms. A calibration is not an A/A session.
        self.aa = args.b is None and args.calibrate is None
        self.calibrating = args.calibrate is not None
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
                self.facts[arm, workload] = read_build_facts(self.executables[arm, workload])
            a, b = self.facts["A", workload], self.facts["B", workload]
            if configuration(a) != configuration(b):
                fail("the toolchain or the configuration of the two arms differs:\n"
                     f"A: {configuration(a)}\nB: {configuration(b)}")
            if library_notice(a, b):
                print(library_notice(a, b), flush=True)
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
        unfinished = ends_mid_line(args.output)
        self.output = args.output.open("a", encoding="utf-8", newline="\n")
        if unfinished:
            self.output.write("\n")     # the next record starts on a line of its own

    def close(self):
        # Reported, not raised: a write that failed in sample() fails here again, and this
        # must not replace that report.
        try:
            self.output.close()
        except OSError as error:
            print(f"run_benchmarks: closing {self.output.name} failed: {error}", file=sys.stderr)
        if not self.keep and self.runs_root.is_dir() and not any(self.runs_root.iterdir()):
            self.runs_root.rmdir()

    def run_swarm(self, executable, cell, run_id, run_dir, timeout):
        """Runs one swarm until the coordinator exits, the timeout expires or an interrupt is
        requested, and cleans up after it in every case, also when the runner itself fails.

        Returns the coordinator's exit code, how the wait for it ended ("exited", "timeout" or
        "interrupted"), and a description of whatever the cleanup left behind or could not
        establish.
        """
        if os.name == "nt":
            participants = Windows_participants(run_id, self.process_tree)
        else:
            participants = Posix_participants(run_id)
        try:
            # The launch and the registration of the ownership are inside the guarded scope,
            # and no signal raises (see request_interrupt), so nothing can separate them.
            participants.launch(cell_command(executable, cell, run_id), run_dir)
            ending = participants.wait_for_coordinator(timeout)
            if ending == "exited":
                wait_for_reports(run_dir)
            remaining = participants.clean_up()
            return participants.process.returncode, ending, remaining
        except BaseException:
            # A failure of the runner itself must not leave the swarm running either.
            remaining = participants.clean_up()
            if remaining:
                print(cleanup_incomplete_message(run_id, run_dir, remaining), file=sys.stderr)
            elif not self.keep:
                remove_run_directory(run_dir)
            raise
        finally:
            participants.close()

    def stop_if_interrupted(self, run_id, run_dir, remaining):
        """Ends the session on an interrupt request. The sample that it struck has no record."""
        if not interrupt_requested:
            return
        if remaining:
            fail("interrupted. " + cleanup_incomplete_message(run_id, run_dir, remaining))
        if not self.keep:
            remove_run_directory(run_dir)
        fail("interrupted. The sample that was running has no record.")

    def sample(self, arm, cell, pair_id=None, order=0):
        """Runs one sample, appends its record to the output and returns it."""
        if interrupt_requested:
            fail("interrupted before the next sample.")
        self.samples += 1
        run_id = f"{self.id}-{self.samples:05d}"
        run_dir = self.runs_root / run_id
        run_dir.mkdir(parents=True)
        executable = self.executables[arm, cell["workload"]]
        facts = self.facts[arm, cell["workload"]]
        ring_entries = len(list(Path(facts["ring_parent_dir"]).glob("sintra-*")))
        # Only a broadcast count is an estimate until it is calibrated. A calibration has
        # to complete a sample at that estimate, however slow the host is.
        estimated = (cell["workload"] == "broadcast" and cell["mode"] == "windowed"
                     and cell["calibration"] is None)
        timeout = CALIBRATION_TIMEOUT_S if self.calibrating else (
            TIMEOUT_BASE_S + cell["duration_s"] * (
                UNCALIBRATED_TIMEOUT_FACTOR if estimated else TIMEOUT_FACTOR))
        started = time.time()
        begin = time.monotonic()
        exit_code, ending, remaining = self.run_swarm(
            executable, cell, run_id, run_dir, timeout)
        wall = time.monotonic() - begin
        timed_out = ending == "timeout"
        self.stop_if_interrupted(run_id, run_dir, remaining)

        try:
            # A swarm that stalled in shutdown has already written its reports, so a timed-out
            # sample is aggregated too. One that stalled earlier has nothing to aggregate.
            reasons = []
            if timed_out:
                reasons.append("timeout")
            elif exit_code != 0:
                reasons.append("exit_code_nonzero")
            if remaining:
                reasons.append("cleanup_incomplete")
            try:
                aggregate = subprocess.run(
                    [str(executable), "--aggregate", str(run_dir)],
                    capture_output=True, text=True, check=False, timeout=AGGREGATE_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                aggregate = None    # subprocess.run() has killed its child and waited for it
            if aggregate is not None and aggregate.returncode == 0:
                record = json.loads(aggregate.stdout)
                if record["run_id"] != run_id:
                    reasons.append("run_id_mismatch")
            else:
                # Also after a timeout: the record says that there is no aggregate, and which
                # reports are absent.
                record = unaggregated_record(cell, run_id, facts, run_dir)
                reasons.append("aggregation_failed")
                if aggregate is None:
                    print(f"--aggregate did not finish within {AGGREGATE_TIMEOUT_S} s",
                          file=sys.stderr)
                else:
                    sys.stderr.write(aggregate.stderr)
            if reasons:
                stderr = (run_dir / "stderr.txt").read_text(encoding="utf-8", errors="replace")
                sys.stderr.write(stderr[-2000:])

            outcome = record["outcome"]
            outcome["reasons"] = reasons + outcome["reasons"]
            outcome["valid"] = not outcome["reasons"]
            if ring_entries:
                outcome["warnings"].append("ring_dir_not_empty")
            if cell.get("not_oversubscribed"):
                outcome["warnings"].append("not_oversubscribed")
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
            # The append is the sample's completion boundary: an interrupt requested before it
            # leaves the sample without a record.
            self.stop_if_interrupted(run_id, run_dir, remaining)
            self.output.write(json.dumps(record) + "\n")
            self.output.flush()
        except Exception:
            # The run directory is kept, and a failure after an incomplete cleanup must not
            # hide what is left.
            print(f"run_benchmarks: the sample of run {run_id} has no record; the run directory "
                  f"{run_dir} is kept.", file=sys.stderr)
            if remaining:
                print(cleanup_incomplete_message(run_id, run_dir, remaining), file=sys.stderr)
            raise
        # The evidence stays while a process that could not be removed may still use it.
        if not self.keep and not remaining:
            remove_run_directory(run_dir)
        print(f"{cell['id']} arm={arm} run={run_id} valid={outcome['valid']} "
              f"reasons={outcome['reasons']} warnings={outcome['warnings']}", flush=True)
        if remaining:
            fail(cleanup_incomplete_message(run_id, run_dir, remaining))
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

def ends_mid_line(path):
    """Whether a results file ends in an unfinished line, as an interrupted write leaves it."""
    if not path.is_file() or path.stat().st_size == 0:
        return False
    with path.open("rb") as data:
        data.seek(-1, os.SEEK_END)
        return data.read(1) != b"\n"


def load_records(path):
    """The records of an earlier results file. A line that does not parse, or that is not a
    record of this schema, is skipped, and the number of such lines is reported."""
    if path is None or not path.is_file():
        return []
    records = []
    skipped = 0
    with path.open(encoding="utf-8", errors="replace") as lines:
        for line in lines:
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except ValueError:
                record = None
            if isinstance(record, dict) and record.get("schema") == SCHEMA:
                records.append(record)
            else:
                skipped += 1
    if skipped:
        print(f"{path}: skipped {skipped} lines that are not {SCHEMA} records", flush=True)
    return records


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
    offered rate, calibration metadata), the toolchain and configuration of its build, host
    label, operating system (host.os) and C.

    The commit, the library constants of the build and the observed counts are not part of
    it. Neither are the framed size and the burst, which follow from the library: a usable
    sample has the default burst, and that is derived from the ring size.
    """
    cell = {key: value for key, value in record["cell"].items()
            if key not in ("framed_bytes", "burst")}
    host = record["host"]
    return json.dumps(
        [cell, configuration(record["build"]), host["label"], host["os"],
         host["cpus_effective"]],
        sort_keys=True)


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
    "lifecycle": [],
}
# The roles whose CPU an open-loop broadcast cell shows: those that do not send. The
# sender's CPU is mostly its generator's clock polling.
OPEN_LOOP_CPU_ROLES = ["coordinator", "receivers"]
SWARM_PHASES = {"formation_cpu": "formation", "shutdown_cpu": "shutdown"}
SHUTDOWN_CALLS = "shutdown calls"


def headline_latencies(cell):
    """The latency objects whose sample support the summary shows for the cell."""
    if cell["workload"] == "rpc":
        return RPC_HEADLINE_LATENCIES[cell["mode"]]
    return HEADLINE_LATENCIES[cell["mode"]]


def headline_metrics(record):
    """(label, always a difference, value) per headline metric of the record's mode.

    CPU and the lost fraction are routinely zero or unavailable, so they are always shown as
    per-arm values with their difference. A value is None when it is unavailable.
    """
    if record["cell"]["workload"] == "rpc":
        return rpc_headline_metrics(record)
    if record["cell"]["mode"] == "lifecycle":
        return swarm_headline_metrics(record)
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
        # The rates and the CPU per message include the fence time. send_call does not: it
        # covers the two payload-header writes and the publish call.
        return metrics + [
            ("send_call p50 us", False, latency["send_call"]["p50_us"]),
            ("slowest_from_send p50 us", False, latency["slowest_from_send"]["p50_us"]),
            ("slowest_from_send p99 us", False, latency["slowest_from_send"]["p99_us"])]
    return [
        ("slowest_from_intended p50 us", False, latency["slowest_from_intended"]["p50_us"]),
        ("slowest_from_intended p99 us", False, latency["slowest_from_intended"]["p99_us"]),
        ("lost fraction", True, lost_fraction(record)),
        ("lateness p99 us", False, latency["lateness"]["p99_us"])] + [
        (f"CPU s {role}", True, cpu_value(record, lambda cpu, role=role: cpu[role]["cpu_s"]))
        for role in OPEN_LOOP_CPU_ROLES if role in record["cpu"]]


def lifecycle_cpu(record, phase, role):
    """A role's CPU seconds over one phase of a swarm sample, or None when a process CPU
    clock did not advance over a phase: the sample's CPU metrics are then unavailable."""
    if "cpu_not_advancing" in record["outcome"]["warnings"]:
        return None
    return record["lifecycle"][phase][role]


def swarm_headline_metrics(record):
    """As headline_metrics, for a swarm sample: the two times, the coordinator's shutdown()
    calls, and each phase's CPU per role."""
    lifecycle = record["lifecycle"]
    metrics = [
        ("time_to_ready s", False, lifecycle["time_to_ready_s"]),
        ("time_to_shutdown s", False, lifecycle["time_to_shutdown_s"]),
        (SHUTDOWN_CALLS, True, lifecycle["shutdown_calls"])]
    for phase, name in SWARM_PHASES.items():
        metrics += [(f"{name} CPU s {role}", True, lifecycle_cpu(record, phase, role))
                    for role in lifecycle[phase]]
    return metrics


def rpc_headline_metrics(record):
    """As headline_metrics, for a request/response sample. Windowed: calls/s, CPU per call in
    total and per role, issue_to_observation. Closed-loop: rtt, CPU per call per role.

    The back-to-back cell also shows calls/s: its rtt distribution has two modes, so p50 can
    move between them from sample to sample while the mean, which calls/s follows, moves less.
    """
    cell = record["cell"]
    windowed = cell["mode"] == "windowed"
    name = RPC_HEADLINE_LATENCIES[cell["mode"]][0]
    latency = [
        (f"{name} {quantile} us", False, record["latency"][name][f"{quantile}_us"])
        for quantile in ("p50", "p99")]
    cpu = [
        (f"CPU us/call {role}", True,
         cpu_value(record, lambda cpu, role=role: cpu[role]["us_per_message"]))
        for role in record["cpu"] if windowed or role != "total"]
    calls = [("calls/s", False, record["throughput"]["calls_s"])]
    if windowed:
        return calls + cpu + latency
    return latency + (calls if cell["gap_us"] == 0 else []) + cpu


def is_single_shutdown_call(label, value_pairs):
    """Whether a summary line would only say that every shutdown() took one call."""
    return label == SHUTDOWN_CALLS and all(value == 1 for pair in value_pairs for value in pair)


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


def compared_as_difference(always_difference, value_pairs):
    """Whether a metric is compared as the difference B-A instead of the ratio B/A: always
    for CPU, the lost fraction and the shutdown calls, and for any metric with an observation
    that is zero or unavailable. The caller passes every pair that the comparison and its A/A
    spread use, so that one representation serves both."""
    return always_difference or any(not value for pair in value_pairs for value in pair)


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


def fence_text(samples):
    """The median time that windowed samples spent in processing fences, and its median
    share of the measured interval. A record of an older executable has no fence time."""
    shares = [(sample["throughput"].get("fence_s"), sample["throughput"]["interval_s"])
              for sample in samples]
    if any(fence is None for fence, _ in shares):
        return "unavailable"
    return (f"{statistics.median(fence for fence, _ in shares):.4g} s, "
            f"{100 * statistics.median(fence / interval for fence, interval in shares):.3g}% "
            "of the interval")


def print_summary(records, aa_records, skipped):
    """Prints the comparison of the session's pairs, cell by cell.

    Beside each comparison stands the spread observed over the comparable A/A pairs in
    `aa_records` that have the same effective parameters. Nothing is flagged.
    """
    print("\n== Summary: median [min, max] of the within-pair comparisons ==")
    print("B/A is a ratio. CPU, the lost fraction, the shutdown calls, and any metric that is")
    print("zero or unavailable in an observation or an A/A observation are shown as per-arm")
    print("medians with B-A instead.")
    print("The A/A spread is the largest deviation within a matching A/A pair, whichever of its")
    print("arms was larger: a factor for a ratio, an absolute value for a difference.")
    builds = {record["arm"]: record["build"] for record in records}
    if len(builds) == 2 and library_notice(builds["A"], builds["B"]):
        print(library_notice(builds["A"], builds["B"]))
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
        for name in headline_latencies(pairs[0][0]["cell"]):
            least = min((sample["latency"][name] for pair in pairs for sample in pair),
                        key=lambda latency: latency["n_messages"])
            print(f"  sample support of {name}: at least n_obs {least['n_obs']}, "
                  f"n_messages {least['n_messages']}")
        cell = pairs[0][0]["cell"]
        if cell["workload"] == "broadcast" and cell["mode"] == "windowed":
            # The windowed broadcast rates and the CPU per message include this time.
            print("  fence time, median: A " + fence_text([a for a, _ in pairs])
                  + "; B " + fence_text([b for _, b in pairs]))
        aa_values = metric_values(aa_pairs_by_key.get(comparison_key(pairs[0][0]), []))
        for label, (always_difference, value_pairs) in metric_values(pairs).items():
            aa_value_pairs = aa_values.get(label, (always_difference, []))[1]
            if is_single_shutdown_call(label, value_pairs + aa_value_pairs):
                continue
            # No observation is dropped; one without a value is counted as unavailable.
            as_difference = compared_as_difference(
                always_difference, value_pairs + aa_value_pairs)
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
            elif as_difference:
                # The arm labels of an A/A pair are arbitrary, so its comparison is folded.
                aa_text = (f"up to {max(abs(difference) for difference in aa_compared):.4g} "
                           + coverage(aa_compared, aa_unavailable))
            else:
                aa_text = (f"up to x{max(max(ratio, 1 / ratio) for ratio in aa_compared):.4g} "
                           + coverage(aa_compared, aa_unavailable))
            print(f"  {label:30} {text}; A/A spread {aa_text}")
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


def fence_percent(sample):
    """The share of a windowed broadcast sample's interval that its sender spent in
    processing fences, in percent. A record of an older executable has no fence time."""
    fence = sample["throughput"].get("fence_s")
    return None if fence is None else 100.0 * fence / sample["throughput"]["interval_s"]


def print_swarm_tables(ids, cells, samples_by_cell, median):
    """W3 against N: the lifecycle times with each role's CPU per leaf, and each role's
    share of a phase's CPU. The coordinator's shutdown() calls are shown when a sample
    needed more than one."""
    def cpu(phase, role, scale):
        def read(sample):
            value = lifecycle_cpu(sample, phase, role)
            return None if value is None else scale(sample, value)
        return read

    def per_leaf_ms(sample, value):
        return 1000.0 * value / sample["cell"]["n_leaves"]

    def percent_of(phase):
        return lambda sample, value: 100.0 * value / sample["lifecycle"][phase]["total"]

    ids = sorted(ids, key=lambda cell_id: cells[cell_id]["n_leaves"])
    lifecycle_columns = {
        "oversubscription": "(N+1)/C", "time_to_ready_s": "ready s",
        "time_to_shutdown_s": "shutdown s"}
    if any("shutdown_retried" in sample["outcome"]["warnings"]
           for cell_id in ids for sample in samples_by_cell[cell_id]):
        lifecycle_columns["shutdown_calls"] = SHUTDOWN_CALLS
    columns = [(phase, role) for phase in SWARM_PHASES
               for role in ("coordinator", "leaves", "total")]
    print_table(
        "Swarm formation and shutdown against N; CPU in ms per leaf",
        ["N"] + list(lifecycle_columns.values())
        + [f"{SWARM_PHASES[phase]} {role}" for phase, role in columns],
        [[cells[cell_id]["n_leaves"]]
         + [median(cell_id, lambda sample, key=key: sample["lifecycle"][key])
            for key in lifecycle_columns]
         + [median(cell_id, cpu(phase, role, per_leaf_ms)) for phase, role in columns]
         for cell_id in ids])
    columns = [(phase, role) for phase in SWARM_PHASES for role in ("coordinator", "leaves")]
    print_table(
        "Share of a swarm phase's CPU by role, percent",
        ["cell"] + [f"{SWARM_PHASES[phase]} {role}" for phase, role in columns],
        [[cell_id] + [median(cell_id, cpu(phase, role, percent_of(phase)))
                      for phase, role in columns]
         for cell_id in ids])


def print_rpc_tables(ids, cells, median, cpu_median):
    """W2 against the gap, for each route: the idle time before each call (actual_gap), rtt,
    and each role's CPU per call. In the reader's sleep phase rtt and actual_gap were seen to
    add up to about a whole number of the reader's poll periods, so the two are read
    together."""
    for route, roles in RPC_ROUTE_ROLES.items():
        of_route = sorted((cell_id for cell_id in ids if cells[cell_id]["route"] == route),
                          key=lambda cell_id: cells[cell_id]["gap_us"])
        if not of_route:
            continue
        latencies = [("actual_gap", "p50_us"), ("rtt", "p50_us"), ("rtt", "p99_us")]
        rows = [[cells[cell_id]["gap_us"]]
                + [median(cell_id, lambda s, name=name, key=key: s["latency"][name][key])
                   for name, key in latencies]
                + [cpu_median(cell_id, lambda cpu, role=role: cpu[role]["us_per_message"])
                   for role in roles + ["total"]]
                for cell_id in of_route]
        print_table(
            f"rtt and CPU us per call by role against the gap: route {route}",
            ["gap us", "actual gap p50 us", "rtt p50 us", "rtt p99 us"] + roles + ["total"],
            rows)


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

    def cell_ids(mode, workload="broadcast", **parameters):
        return [cell_id for cell_id, cell in cells.items()
                if cell["workload"] == workload and cell["mode"] == mode
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
                + [cpu_median(cell_id, lambda cpu: cpu["receivers"]["us_per_delivery"]),
                   median(cell_id, fence_percent)]
                for cell_id in sorted(ids, key=lambda cell_id: cells[cell_id]["receivers"])]
        print_table(
            f"CPU us per message by role against R: origin {origin}, payload {payload} B",
            ["R"] + roles + ["receivers us/delivery", "fence %"], rows)

    # The CPU of a windowed sample includes the CPU of its fence time, so each value stands
    # beside the share of the interval that the sender spent in fences.
    for (origin,) in axis("windowed", "origin"):
        receivers = [r for (r,) in axis("windowed", "receivers")]
        rows = []
        for (payload,) in axis("windowed", "payload_bytes"):
            row = [payload]
            for count in receivers:
                ids = cell_ids("windowed", origin=origin, payload_bytes=payload, receivers=count)
                row += [median(ids[0], read) if ids else None
                        for read in (cpu_us_per_megabyte, fence_percent)]
            rows.append(row)
        print_table(
            f"Total CPU us per application MB against payload: origin {origin}",
            ["payload B"] + [title for count in receivers
                             for title in (f"R={count}", f"R={count} fence %")], rows)

    for origin, count, payload in axis("open_loop", "origin", "receivers", "payload_bytes"):
        ids = cell_ids("open_loop", origin=origin, receivers=count, payload_bytes=payload)
        roles = [role for role in OPEN_LOOP_CPU_ROLES if role in samples_by_cell[ids[0]][0]["cpu"]]
        rows = [[cells[cell_id]["offered_rate"],
                 median(cell_id, lambda s: s["latency"]["slowest_from_intended"]["p50_us"]),
                 median(cell_id, lambda s: s["latency"]["slowest_from_intended"]["p99_us"]),
                 median(cell_id, lost_fraction),
                 median(cell_id, lambda s: s["latency"]["lateness"]["p99_us"])]
                + [cpu_median(cell_id, lambda cpu, role=role: cpu[role]["cpu_s"])
                   for role in roles]
                for cell_id in sorted(ids, key=lambda cell_id: cells[cell_id]["offered_rate"])]
        print_table(
            f"Open-loop latency and loss against offered rate: origin {origin}, R={count}, "
            f"payload {payload} B",
            ["offered msg/s", "slowest p50 us", "slowest p99 us", "lost fraction",
             "lateness p99 us"] + [f"{role} CPU s" for role in roles], rows)

    def share(role):
        def read(cpu):
            if role not in cpu:
                return None
            return 100.0 * cpu[role]["cpu_s"] / cpu["total"]["cpu_s"]
        return read

    # The sender CPU of an open-loop cell is mostly its generator's clock polling, so the
    # shares are taken from the windowed cells only.
    windowed = cell_ids("windowed")
    if windowed:
        print_table(
            "Share of total CPU by role, percent: windowed cells", ["cell"] + ROLES,
            [[cell_id] + [cpu_median(cell_id, share(role)) for role in ROLES]
             for cell_id in windowed])

    # A closed-loop request/response sample is read against its gap, in its own tables.
    rpc_ids = cell_ids("closed_loop", workload="rpc")
    if rpc_ids:
        print_rpc_tables(rpc_ids, cells, median, cpu_median)

    # A swarm sample has its CPU per phase, in its own tables.
    swarm_ids = cell_ids("lifecycle", workload="swarm")
    if swarm_ids:
        print_swarm_tables(swarm_ids, cells, samples_by_cell, median)


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
                        help="JSONL file with more A/A records for the A/A spread")
    parser.add_argument("--calibrate", type=Path, metavar="NEW_WORKLOADS",
                        help="calibrate the selected windowed cells into this workloads file")
    parser.add_argument("--keep", action="store_true", help="keep the run directories")
    parser.add_argument("--require-valid", action="store_true",
                        help="exit non-zero when a record of this session is invalid")
    args = parser.parse_args()
    if args.b is not None and args.b_commit is None:
        parser.error("--b needs --b-commit")
    if args.require_valid and args.calibrate is not None:
        parser.error("--require-valid does not apply to --calibrate")
    return args


def require_valid(records):
    """Fails the session when one of its records is invalid. A warning does not count."""
    invalid = [record for record in records if not record["outcome"]["valid"]]
    if invalid:
        fail(f"{len(invalid)} of {len(records)} records are invalid:\n  " + "\n  ".join(
            f"{record['cell']['id']} arm={record['arm']} run={record['run_id']} "
            f"reasons={record['outcome']['reasons']}" for record in invalid))


def lacks_rate(cell):
    return cell["mode"] == "open_loop" and cell["messages"] is None


def main():
    # A request to stop only sets a flag: see request_interrupt. A signal that the runner
    # was started to ignore, as under nohup, stays ignored.
    for name in ("SIGINT", "SIGTERM", "SIGHUP", "SIGQUIT", "SIGBREAK"):
        number = getattr(signal, name, None)
        if number is not None and signal.getsignal(number) != signal.SIG_IGN:
            signal.signal(number, request_interrupt)

    args = parse_arguments()
    workloads = json.loads(args.workloads.read_text(encoding="utf-8"))
    validate_workloads(workloads)
    validate_swarm(workloads)
    cells = select_cells(expand_cells(workloads) + expand_swarm_cells(workloads), args.select)
    if args.calibrate:
        # Calibration is for the windowed broadcast cells. Request/response counts are fixed.
        cells = [cell for cell in cells
                 if cell["workload"] == "broadcast" and cell["mode"] == "windowed"]
    # An open-loop cell has no message count until calibration has stored its rate.
    skipped = [cell for cell in cells if lacks_rate(cell)]
    cells = [cell for cell in cells if not lacks_rate(cell)]
    if skipped:
        print("skipped, no stored open-loop rate: " + ", ".join(cell["id"] for cell in skipped))
    if not cells:
        fail("no cell to run")

    earlier_aa = [record for record in load_records(args.output) + load_records(args.aa)
                  if record.get("aa")]
    session = Session(args, cells)
    try:
        cells, unsized = size_swarm_cells(
            cells, workloads.get("swarm"), session.host["cpus_effective"])
        report_unsized(unsized)
        if not cells:
            fail("no cell to run")
        if args.calibrate:
            calibrate(session, cells, workloads, args.calibrate)
            return
        records = run_pairs(session, cells, args.pairs)
    finally:
        session.close()
    # The A/A records of the output file include this session's own, if it is an A/A session.
    print_summary(records, earlier_aa + (records if session.aa else []), skipped)
    report_unsized(unsized)
    print_axis_tables(records)
    if args.require_valid:
        require_valid(records)


if __name__ == "__main__":
    main()
