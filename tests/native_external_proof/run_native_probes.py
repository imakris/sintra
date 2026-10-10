#!/usr/bin/env python3
"""Isolated native observations. Never treats a failed compile/signal as N3 support."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time

def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def execute(command, timeout=30, cwd=None):
    started = utc()
    process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               start_new_session=(os.name == "posix"))
    timed_out = False
    cleanup_error = None
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        # communicate has not consumed this child: its PID still reserves the
        # process-group identity. Every fixture descendant stays in this group.
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired as incomplete:
            cleanup_error = "owned process-group pipes did not settle after SIGKILL"
            process.stdout.close()
            process.stderr.close()
            stdout = incomplete.output or ""
            stderr = incomplete.stderr or ""
            if isinstance(stdout, bytes): stdout = stdout.decode(errors="replace")
            if isinstance(stderr, bytes): stderr = stderr.decode(errors="replace")
            stderr += "\n" + cleanup_error
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                cleanup_error += "; root child did not settle"
    return {"command": list(map(str, command)), "started_utc": started,
            "finished_utc": utc(), "returncode": process.returncode,
            "timed_out": timed_out, "cleanup_error": cleanup_error,
            "stdout": stdout, "stderr": stderr}

def records(result):
    rows = []
    for line in result["stdout"].splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(item, dict):
            rows.append(item)
    return rows

# B1 remedy/outcome table follows the attributed sibling fixture review.
# Any recorded success dominates subsequent exit/signal/timeout observations.
_DARWIN_NEGATIVE_STAGES = {
    "dup": ("attempt_guarded_dup", "guarded_dup_returned"),
    "send_rights": ("attempt_guarded_scm_rights", "guarded_scm_rights_returned"),
    "fileport": ("attempt_guarded_fileport", "guarded_fileport_returned"),
}

def classify_darwin_negative(case, outcome):
    attempt_stage, operation_stage = _DARWIN_NEGATIVE_STAGES[case]
    rows = outcome.get("records", [])
    attempts = [i for i, row in enumerate(rows) if row.get("stage") == attempt_stage]
    operations = [(i, row) for i, row in enumerate(rows)
                  if row.get("stage") == operation_stage]
    code = outcome.get("returncode")
    result = {"classification": None, "gate_completed": False,
              "observed_violation": False, "operation_records": [row for _, row in operations],
              "termination_signal": -code if type(code) is int and code < 0 else None,
              "rejection_cause": "unproved"}

    def conclude(label, completed=False, violation=False):
        result.update(classification=label, gate_completed=completed,
                      observed_violation=violation)
        return result

    # The return record is a native observation even if later cleanup fails.
    for _, row in operations:
        value = row.get("return")
        if type(value) is int and ((case == "fileport" and value == 0)
                                  or (case != "fileport" and value >= 0)):
            return conclude("returned_success_violation", violation=True)
    if code == 23:
        return conclude("fixture_reported_violation", violation=True)
    if outcome.get("timed_out"):
        return conclude("diagnostic_timeout")
    if outcome.get("cleanup_error"):
        return conclude("fixture_cleanup_unsettled")
    if not attempts:
        if type(code) is int and code < 0:
            return conclude("terminated_before_attempt_unclassified")
        return conclude({20: "setup_unavailable", 21: "setup_failure"}.get(
            code, "case_not_attempted"))
    if not operations:
        if type(code) is int and code < 0:
            return conclude("terminated_after_attempt_unclassified")
        return conclude({20: "runtime_unavailable", 21: "fixture_failure"}.get(
            code, "missing_operation_return"))
    if len(operations) != 1:
        return conclude("multiple_operation_returns_unresolved")
    index, operation = operations[0]
    value, error = operation.get("return"), operation.get("errno")
    if type(value) is not int or type(error) is not int or value >= 0 or error <= 0:
        return conclude("operation_return_unrecognized")
    if attempts[0] >= index:
        return conclude("operation_order_unresolved")
    if type(code) is int and code < 0:
        return conclude("returned_rejection_then_termination_unclassified")
    if code is None:
        return conclude("returned_rejection_without_settled_process")
    if code != 0:
        return conclude("returned_rejection_then_fixture_failure")
    return conclude("returned_rejection", completed=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hosted-ci", action="store_true")
    parser.add_argument("--output", required=True)
    parser.add_argument("--expected-arch")
    args = parser.parse_args()
    hosted = args.hosted_ci and os.environ.get("GITHUB_ACTIONS") == "true"
    if args.hosted_ci and not hosted:
        parser.error("--hosted-ci requires actual GITHUB_ACTIONS=true")
    source = Path(__file__).resolve().parent
    destination = Path(args.output).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        parser.error("output already exists; preserve previous evidence")
    result = {"started_utc": utc(), "runtime": {"system": platform.system(),
              "release": platform.release(), "version": platform.version(),
              "machine": platform.machine(), "python": sys.version},
              "hosted_ci": hosted, "source_hashes": {}, "commands": [],
              "n3_acceptance": False, "exact_peer_contract": "unproved",
              "pid_reuse_exercised": False, "sintra_process_word_exercised": False}
    for name in ("linux_probe.py", "freebsd_probe.c", "darwin_probe.c",
                 "darwin_link_probe.c", "darwin_public_header_probe.c",
                 "run_native_probes.py"):
        result["source_hashes"][name] = hashlib.sha256((source/name).read_bytes()).hexdigest()
    status = 1

    def run(command, timeout=30, cwd=None):
        row = execute(command, timeout, cwd)
        row["records"] = records(row)
        result["commands"].append(row)
        return row

    def build_command(command):
        # Hosted execution follows the repository's already authorized native
        # CI venue. Every local compiler/link invocation uses the real queue.
        if hosted:
            return command
        queue = shutil.which("queued-build")
        if queue is None:
            raise RuntimeError("real queued-build unavailable for local compilation")
        return [queue, "--slots", "1", "--", *command]

    try:
        if args.expected_arch and platform.machine() != args.expected_arch:
            raise RuntimeError("actual native architecture differs from requested runner")
        system = platform.system()
        if system == "Linux":
            row = run([sys.executable, "-B", str(source/"linux_probe.py")], 30)
            supported = any(x.get("status") == "supported_observed" for x in row["records"])
            status = 0 if row["returncode"] == 0 and supported and not row["timed_out"] else 1
        elif system in ("Darwin", "FreeBSD"):
            compiler = shutil.which("clang" if system == "Darwin" else "cc")
            if not compiler:
                raise RuntimeError("native C compiler unavailable")
            run(build_command([compiler, "--version"]))
            with tempfile.TemporaryDirectory(prefix="n3-native-") as scratch:
                scratch = Path(scratch)
                if system == "FreeBSD":
                    binary = scratch/"freebsd-native"
                    built = run(build_command([compiler, "-std=gnu11", "-Wall", "-Wextra",
                                "-O0", str(source/"freebsd_probe.c"), "-o", str(binary)]), 90)
                    if built["returncode"] == 0 and not built["timed_out"]:
                        row = run([str(binary)], 35)
                        result["freebsd_observations"] = row["records"]
                        supported = any(x.get("status") == "supported_observed"
                                        and x.get("n3_acceptance") is False
                                        and x.get("cleanup_result") == 0 for x in row["records"])
                        status = 0 if row["returncode"] == 0 and supported and not row["timed_out"] else 1
                    else:
                        result["native_compile_failed"] = True
                else:
                    result["deployment_target_requested"] = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
                    run(["sw_vers"])
                    run(["xcrun", "--show-sdk-path"])
                    run(["xcrun", "--show-sdk-version"])
                    # Negative SDK evidence stays distinct from runtime capability.
                    for name in ("darwin_public_header_probe.c", "darwin_link_probe.c"):
                        built = run(build_command([compiler, "-std=c11", "-Wall", "-Wextra",
                            "-Werror=implicit-function-declaration", "-O0", str(source/name),
                            "-o", str(scratch/Path(name).stem)]), 90)
                        result.setdefault("sdk_checks", {})[name] = {
                            "compiled_and_linked": built["returncode"] == 0 and not built["timed_out"],
                            "runtime_executed": False, "vendor_support_proved": False}
                    binary = scratch/"darwin-native"
                    built = run(build_command([compiler, "-std=c11", "-Wall", "-Wextra",
                                "-O0", str(source/"darwin_probe.c"), "-o", str(binary)]), 90)
                    if built["returncode"] == 0 and not built["timed_out"]:
                        run(["otool", "-l", str(binary)])
                        cases = {}
                        for case in ("baseline", "dup", "send_rights", "fileport",
                                     "fork", "raw_fork", "native_exit"):
                            with tempfile.TemporaryDirectory(prefix="n3-d-", dir="/tmp") as directory:
                                row = run([str(binary), case, directory], 20)
                            cases[case] = {"returncode": row["returncode"],
                                "signal_observed": -row["returncode"] if row["returncode"] is not None
                                                   and row["returncode"] < 0 else None,
                                "signal_classification": "unclassified",
                                "timed_out": row["timed_out"], "records": row["records"]}
                        result["darwin_cases"] = cases
                        positives = all(cases[name]["returncode"] == 0 and not cases[name]["timed_out"]
                                        for name in ("baseline", "fork", "raw_fork", "native_exit"))
                        baseline = cases["baseline"]["records"]
                        setup = all(any(x.get("stage") == stage and x.get("return") == expected
                                        for x in baseline)
                                    for stage, expected in (("install_private_guards", 0),
                                        ("set_confined", 0), ("get_confined", 1),
                                        ("matching_client_close", 0)))
                        negative_outcomes = {
                            name: classify_darwin_negative(name, cases[name])
                            for name in ("dup", "send_rights", "fileport")}
                        result["darwin_negative_outcomes"] = negative_outcomes
                        result["causal_negative_gates_completed"] = all(
                            item["gate_completed"] for item in negative_outcomes.values())
                        result["raw_evidence_collected"] = True
                        status = 0 if positives and setup and result["causal_negative_gates_completed"] else 1
                        result["private_spi_compatibility"] = "unproved"
                    else:
                        result["native_compile_failed"] = True
        else:
            raise RuntimeError("no probe for this execution venue")
        result["status"] = "native_observations_captured" if status == 0 else "native_gate_not_completed"
    except Exception as error:
        result["status"] = "probe_error"
        result["error"] = str(error)
        status = 1
    result["finished_utc"] = utc()
    result["exit_status"] = status
    with destination.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
    print(json.dumps({"output": str(destination), "status": result["status"],
                      "n3_acceptance": False}))
    return status

if __name__ == "__main__":
    sys.exit(main())
