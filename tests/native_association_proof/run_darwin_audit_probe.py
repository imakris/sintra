#!/usr/bin/env python3
"""Collect actual standard-header Darwin native evidence, without N3 acceptance."""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import tempfile


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def case_completed(outcome, expected_case):
    if outcome.get("returncode") != 0 or outcome.get("timed_out") or outcome.get("cleanup_error"):
        return False
    rows = outcome.get("records", [])
    # Native cleanup failure records remain authoritative even if later code
    # accidentally emits a success summary or exits zero.
    cleanup_operations = [row for row in rows
                          if str(row.get("stage", "")).startswith("cleanup_")]
    if any(row.get("return") != 0 or row.get("error") != 0 for row in cleanup_operations):
        return False
    checked = [row for row in rows if row.get("stage") == "checked_cleanup_summary"]
    if len(checked) != 1 or checked[0].get("failures") != 0:
        return False
    def exactly(stage):
        selected = [row for row in rows if row.get("stage") == stage]
        return selected[0] if len(selected) == 1 else None
    summary = exactly("capability_observed")
    cleanup = exactly("fixture_cleanup_complete")
    reap = exactly("owned_child_consumed")
    installed = exactly("native_exit_filter_armed")
    exit_observed = exactly("exact_owned_child_exit_observed")
    handoff = exactly("received_send_right_handoff")
    if not all((summary, cleanup, reap, installed, exit_observed, handoff)):
        return False
    required = ("standard_header_decoder_executed", "fresh_sender_matched_self",
                "post_arm_same_incarnation", "send_right_handoff",
                "native_exit_observed", "owned_child_consumed")
    if (summary.get("case") != expected_case or cleanup.get("return") != 1
            or not all(summary.get(name) is True for name in required)
            or summary.get("n3_acceptance") is not False
            or summary.get("pid_reuse_exercised") is not False
            or installed.get("return") != 0
            or type(handoff.get("return")) is not int or handoff["return"] <= 0):
        return False
    senders = [row for row in rows if row.get("stage") == "sender_identity"]
    if len(senders) != 4:
        return False
    by_stage = {}
    for row in senders:
        stage = row.get("message_stage")
        pid, version = row.get("kernel_pid"), row.get("kernel_version")
        if (type(stage) is not int or stage not in (1, 2, 3, 4) or stage in by_stage
                or type(pid) is not int or pid <= 0 or type(version) is not int
                or row.get("self_pid") != pid or row.get("self_version") != version):
            return False
        by_stage[stage] = (pid, version)
    if (by_stage[1] != by_stage[3] or by_stage[2] != by_stage[4]
            or by_stage[1][0] == by_stage[2][0]):
        return False
    if (reap.get("pid") != by_stage[1][0] or reap.get("normal_exit") is not True
            or reap.get("exit_code") != 0 or reap.get("signal") != 0
            or type(reap.get("raw_status")) is not int):
        return False
    # Judge the native receipt, retaining the independent derived result.
    # Expected filter/flag values come from the actual SDK macros emitted by C;
    # the driver does not invent an audit-token or kevent ABI layout.
    numeric = ("return", "error", "expected_ident", "expected_filter",
               "note_exit_flag", "event_error_flag", "ident", "filter",
               "flags", "fflags", "data")
    if any(type(exit_observed.get(field)) is not int for field in numeric):
        return False
    if (exit_observed["return"] != 1 or exit_observed["error"] != 0
            or exit_observed.get("observed") is not True
            or exit_observed.get("event_present") is not True
            or exit_observed["expected_ident"] != by_stage[1][0]
            or exit_observed["ident"] != by_stage[1][0]
            or exit_observed["filter"] != exit_observed["expected_filter"]
            or exit_observed["note_exit_flag"] <= 0
            or exit_observed["event_error_flag"] <= 0
            or not (exit_observed["fflags"] & exit_observed["note_exit_flag"])
            or (exit_observed["flags"] & exit_observed["event_error_flag"])):
        return False
    for operation in ("mach_send", "mach_receive"):
        operations = [row for row in rows if row.get("stage") == operation]
        if len(operations) != 4 or any(row.get("return") != 0 for row in operations):
            return False
    return True


def capability_completed(public_built, diagnostic_built, cases):
    # Ordinary public-header evidence cannot stand in for the raw-fork case.
    return (public_built is True and diagnostic_built is True
            and set(cases) == {"fork", "raw_fork"}
            and all(case_completed(row, case) for case, row in cases.items()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hosted-ci", action="store_true")
    parser.add_argument("--expected-arch", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--shared-driver")
    args = parser.parse_args()
    hosted = args.hosted_ci and os.environ.get("GITHUB_ACTIONS") == "true"
    if args.hosted_ci and not hosted:
        parser.error("--hosted-ci requires the actual authorized GitHub runner")
    source = Path(__file__).resolve().parent
    destination = Path(args.output).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        parser.error("output exists; preserve earlier evidence")
    shared = (Path(args.shared_driver).resolve() if args.shared_driver else
              source.parent / "native_external_proof" / "run_native_probes.py")
    specification = importlib.util.spec_from_file_location("retained_native_driver", shared)
    retained = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(retained)
    result = {
        "started_utc": utc(), "source_hashes": {}, "commands": [], "hosted_ci": hosted,
        "runtime": {"system": platform.system(), "release": platform.release(),
                    "version": platform.version(), "architecture": platform.machine()},
        "deployment_target_requested": os.environ.get("MACOSX_DEPLOYMENT_TARGET"),
        "native_capability_gate_completed": False, "public_sdk_support_conclusion": "unproved",
        "standard_header_compile_and_link": False,
        "raw_fork_diagnostic_compile_and_link": False, "cases": {},
        "n3_acceptance": False, "pid_reuse_exercised": False,
        "sintra_process_word_exercised": False,
        "raw_fork_trigger": {
            "diagnostic_only": True,
            "native_authority": False,
            "sdk_selector_header": "sys/syscall.h",
            "private_selector_exposure": "diagnostic_variant_only",
            "prior_sdk_15_5_warning": "syscall() is unsupported",
            "vendor_support_conclusion": "not_claimed",
            "manual_private_prototype": False,
            "hardcoded_syscall_number": False,
        },
    }
    for path in (source / "darwin_audit_probe.c", Path(__file__).resolve(), shared):
        result["source_hashes"][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()

    def run(command, timeout=30):
        row = retained.execute(command, timeout)
        row["records"] = retained.records(row)
        result["commands"].append(row)
        return row

    def compiler_command(command):
        if hosted:
            return command
        queue = shutil.which("queued-build")
        if not queue:
            raise RuntimeError("real queued-build unavailable for local compiler/link command")
        return [queue, "--slots", "1", "--", *command]

    status = 1
    try:
        if platform.system() != "Darwin" or platform.machine() != args.expected_arch:
            raise RuntimeError("requested native Darwin architecture is unavailable here")
        compiler = shutil.which("clang")
        if not compiler:
            raise RuntimeError("native clang unavailable")
        run(compiler_command([compiler, "--version"]))
        run(["sw_vers"])
        sdk = run(["xcrun", "--show-sdk-path"])
        run(["xcrun", "--show-sdk-version"])
        if sdk["returncode"] != 0 or sdk["timed_out"]:
            raise RuntimeError("actual SDK path unavailable")
        headers = destination.parent / "sdk_headers"
        headers.mkdir(exist_ok=False)
        sdk_root = Path(sdk["stdout"].strip())
        for relative in ("usr/include/bsm/libbsm.h", "usr/include/sys/appleapiopts.h",
                         "usr/include/mach/message.h", "usr/include/mach/mach_traps.h",
                         "usr/include/sys/un.h", "usr/include/sys/syscall.h",
                         "usr/include/unistd.h"):
            native_header = sdk_root / relative
            if not native_header.is_file():
                result.setdefault("sdk_header_missing", []).append(relative)
                continue
            body = native_header.read_bytes()
            saved = headers / Path(relative).name
            saved.write_bytes(body)
            result.setdefault("sdk_header_hashes", {})[relative] = hashlib.sha256(body).hexdigest()
        with tempfile.TemporaryDirectory(prefix="n3-audit-") as temporary:
            cases = result["cases"]
            builds = {}
            for case, diagnostic in (("fork", False), ("raw_fork", True)):
                binary = Path(temporary) / ("darwin-audit-" + case)
                command = [compiler, "-std=c11", "-Wall", "-Wextra",
                           "-Werror=implicit-function-declaration", "-O0",
                           "-arch", args.expected_arch, "-isysroot", str(sdk_root)]
                if diagnostic:
                    command.append("-DN3_DIAGNOSTIC_RAW_FORK=1")
                command += [str(source / "darwin_audit_probe.c"), "-lbsm", "-o", str(binary)]
                built = run(compiler_command(command), 90)
                builds[case] = (built["returncode"] == 0 and not built["timed_out"]
                                and not built["cleanup_error"])
                key = ("raw_fork_diagnostic_compile_and_link" if diagnostic
                       else "standard_header_compile_and_link")
                result[key] = builds[case]
                if builds[case]:
                    run(["otool", "-hv", str(binary)])
                    run(["otool", "-l", str(binary)])
                    cases[case] = run([str(binary), case], 20)
            result["standard_header_compile_and_link"] = builds["fork"]
            result["raw_fork_diagnostic_compile_and_link"] = builds["raw_fork"]
            result["cases"] = cases
        # Collection is complete only after temporary resources settle.
        result["native_capability_gate_completed"] = capability_completed(
            builds["fork"], builds["raw_fork"], cases)
        status = 0 if result["native_capability_gate_completed"] else 1
    except Exception as error:
        status = 1
        result["native_capability_gate_completed"] = False
        result["gate_error"] = str(error)
    result["finished_utc"] = utc()
    result["status"] = ("native_capability_observed" if status == 0 else "native_gate_not_completed")
    destination.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return status


if __name__ == "__main__":
    sys.exit(main())
