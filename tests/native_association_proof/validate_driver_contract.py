"""Source-derived synthetic oracle faults; never native platform observations."""
import argparse
import ast
import hashlib
import copy
import importlib.util
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--output")
parser.add_argument("--historical-driver")
parser.add_argument("--shared-driver", help="explicit used dependency for flat evidence packets")
args = parser.parse_args()
source = Path(__file__).resolve().parent
shared = (Path(args.shared_driver).resolve() if args.shared_driver else
          source.parent / "native_external_proof" / "run_native_probes.py")
destination = Path(args.output).resolve() if args.output else source / "driver_contract_validation_r3.json"
if destination.exists():
    parser.error("output exists; preserve earlier oracle evidence")
destination.parent.mkdir(parents=True, exist_ok=True)
driver = source / "run_darwin_audit_probe.py"
ast.parse(driver.read_text(encoding="utf-8"))
specification = importlib.util.spec_from_file_location("audit_capability_driver", driver)
module = importlib.util.module_from_spec(specification)
specification.loader.exec_module(module)
summary = {"stage": "capability_observed", "case": "raw_fork",
           "standard_header_decoder_executed": True, "fresh_sender_matched_self": True,
           "post_arm_same_incarnation": True, "send_right_handoff": True,
           "native_exit_observed": True, "owned_child_consumed": True,
           "n3_acceptance": False, "pid_reuse_exercised": False}
rows = [summary, {"stage": "fixture_cleanup_complete", "return": 1},
        {"stage": "checked_cleanup_summary", "failures": 0},
        {"stage": "cleanup_self_socket_0", "return": 0, "error": 0},
        {"stage": "cleanup_self_socket_1", "return": 0, "error": 0},
        {"stage": "owned_child_consumed", "pid": 200, "raw_status": 0,
         "normal_exit": True, "exit_code": 0, "signal": 0},
        {"stage": "native_exit_filter_armed", "return": 0},
        {"stage": "exact_owned_child_exit_observed", "return": 1, "error": 0,
         "observed": True, "event_present": True, "expected_ident": 200,
         "expected_filter": -77, "note_exit_flag": 256, "event_error_flag": 16,
         "ident": 200, "filter": -77, "flags": 0, "fflags": 256, "data": 0},
        {"stage": "received_send_right_handoff", "return": 123}]
for stage, pid, version in ((1, 200, 7), (2, 100, 6), (3, 200, 7), (4, 100, 6)):
    rows.append({"stage": "sender_identity", "message_stage": stage,
                 "kernel_pid": pid, "kernel_version": version,
                 "self_pid": pid, "self_version": version})
for operation in ("mach_send", "mach_receive"):
    rows.extend({"stage": operation, "return": 0} for _ in range(4))
positive = {"returncode": 0, "timed_out": False, "cleanup_error": None, "records": rows}
vectors = [("complete_synthetic_receipt", positive, True)]
for name, key, value in (("signal_after_records", "returncode", -9),
                         ("fixture_failure_after_records", "returncode", 21),
                         ("supervisor_deadline", "timed_out", True),
                         ("unsettled_process_group", "cleanup_error", "pipes remain")):
    changed = copy.deepcopy(positive)
    changed[key] = value
    vectors.append((name, changed, False))
for name, field in (("no_sender_match", "fresh_sender_matched_self"),
                    ("different_post_arm_incarnation_summary", "post_arm_same_incarnation"),
                    ("no_native_exit", "native_exit_observed"),
                    ("no_owned_reap", "owned_child_consumed")):
    changed = copy.deepcopy(positive)
    changed["records"][0][field] = False
    vectors.append((name, changed, False))
for name, stage, key, value in (
        ("wrong_case_receipt", "capability_observed", "case", "fork"),
        ("signalled_owned_child", "owned_child_consumed", "signal", 9),
        ("wrong_owned_child_reaped", "owned_child_consumed", "pid", 201),
        ("native_filter_rejected", "native_exit_filter_armed", "return", -1),
        ("failed_right_handoff", "received_send_right_handoff", "return", 0),
        ("failed_mach_send", "mach_send", "return", 5),
        ("failed_mach_receive", "mach_receive", "return", 6)):
    changed = copy.deepcopy(positive)
    row = next(row for row in changed["records"] if row["stage"] == stage)
    row[key] = value
    vectors.append((name, changed, False))
for name, changed_stage, field, value in (
        ("kernel_self_pid_disagree", 1, "self_pid", 999),
        ("kernel_self_version_disagree", 1, "self_version", 999),
        ("confirm_other_current_holder", 3, "kernel_pid", 201),
        ("confirm_other_incarnation", 3, "kernel_version", 8),
        ("release_other_coordinator", 4, "kernel_pid", 101)):
    changed = copy.deepcopy(positive)
    row = next(row for row in changed["records"]
               if row["stage"] == "sender_identity" and row["message_stage"] == changed_stage)
    row[field] = value
    if field == "kernel_pid":
        row["self_pid"] = value
    if field == "kernel_version":
        row["self_version"] = value
    vectors.append((name, changed, False))
for stage in ("fixture_cleanup_complete", "owned_child_consumed", "sender_identity"):
    changed = copy.deepcopy(positive)
    changed["records"].remove(next(row for row in changed["records"] if row["stage"] == stage))
    vectors.append(("missing_"+stage, changed, False))
changed = copy.deepcopy(positive)
changed["records"].append(copy.deepcopy(summary))
vectors.append(("duplicate_summary", changed, False))
for name, row in (
        ("socket_close_failed_after_success_query",
         {"stage": "cleanup_self_socket_0", "return": -1, "error": 9}),
        ("mach_cleanup_failed_after_success_summary",
         {"stage": "cleanup_child_reply_receive", "return": 5, "error": 5}),
        ("vm_cleanup_failed_after_success_summary",
         {"stage": "cleanup_child_lookup_vm", "return": 1, "error": 1}),
        ("cleanup_error_even_when_return_zero",
         {"stage": "cleanup_parent_queue", "return": 0, "error": 5}),
        ("cleanup_outcome_missing_native_error",
         {"stage": "cleanup_parent_queue", "return": -1})):
    changed = copy.deepcopy(positive)
    changed["records"].append(row)
    vectors.append((name, changed, False))
changed = copy.deepcopy(positive)
changed["records"].remove(next(row for row in changed["records"]
                              if row["stage"] == "checked_cleanup_summary"))
vectors.append(("missing_checked_cleanup_summary", changed, False))
changed = copy.deepcopy(positive)
next(row for row in changed["records"] if row["stage"] == "checked_cleanup_summary")["failures"] = 1
vectors.append(("aggregate_cleanup_failure_after_success", changed, False))
# These event constants/fields are synthetic inputs, not Darwin ABI claims.
for name, key, value in (
        ("native_exit_return_negative", "return", -1),
        ("native_exit_timeout_count_zero", "return", 0),
        ("native_exit_unexpected_count", "return", 2),
        ("native_exit_errno_after_success_summary", "error", 77),
        ("native_exit_derived_match_false", "observed", False),
        ("native_exit_no_returned_event", "event_present", False),
        ("native_exit_wrong_expected_child", "expected_ident", 201),
        ("native_exit_wrong_returned_child", "ident", 201),
        ("native_exit_wrong_returned_filter", "filter", -78),
        ("native_exit_missing_note_exit", "fflags", 0),
        ("native_exit_error_flag_even_with_match", "flags", 16)):
    changed = copy.deepcopy(positive)
    row = next(row for row in changed["records"]
               if row["stage"] == "exact_owned_child_exit_observed")
    row[key] = value
    vectors.append((name, changed, False))
for field in ("error", "ident", "filter", "flags", "fflags", "data"):
    changed = copy.deepcopy(positive)
    row = next(row for row in changed["records"]
               if row["stage"] == "exact_owned_child_exit_observed")
    del row[field]
    vectors.append(("native_exit_missing_" + field, changed, False))
outcomes = []
for name, outcome, expected in vectors:
    actual = module.case_completed(outcome, "raw_fork")
    outcomes.append({"case": name, "expected": expected, "actual": actual, "passed": actual == expected})
# Complete collection requires both separately built variants and both cases.
# These are source-bound synthetic outcomes; no compiler or Darwin native API
# executes in this validator.
ordinary = copy.deepcopy(positive)
ordinary["records"][0]["case"] = "fork"
complete_cases = {"fork": ordinary, "raw_fork": positive}
aggregate_vectors = [
    ("both_separate_builds_and_cases", True, True, complete_cases, True),
    ("missing_raw_case", True, True, {"fork": ordinary}, False),
    ("missing_public_case", True, True, {"raw_fork": positive}, False),
    ("empty_cases_not_vacuous_success", True, True, {}, False),
    ("public_build_failed", False, True, complete_cases, False),
    ("diagnostic_build_failed", True, False, complete_cases, False),
    ("numeric_public_build_not_true", 1, True, complete_cases, False),
    ("numeric_diagnostic_build_not_true", True, 1, complete_cases, False),
]
for name, case, field, value in (
        ("raw_signal_after_complete_receipts", "raw_fork", "returncode", -9),
        ("ordinary_unsettled_cleanup", "fork", "cleanup_error", "pipes remain")):
    changed = copy.deepcopy(complete_cases)
    changed[case][field] = value
    aggregate_vectors.append((name, True, True, changed, False))
changed = copy.deepcopy(complete_cases)
changed["raw_fork"]["records"][0]["case"] = "fork"
aggregate_vectors.append(("raw_case_cannot_be_ordinary_substitute", True, True, changed, False))
for name, public_built, diagnostic_built, cases, expected in aggregate_vectors:
    actual = module.capability_completed(public_built, diagnostic_built, cases)
    outcomes.append({"case": name, "expected": expected, "actual": actual,
                     "passed": actual == expected})


def collector_flow(driver_path, shared_driver, fault):
    """Execute actual collector Python under explicitly synthetic native inputs.

    Compiler/probe commands are intercepted, never executed. The used real
    records parser still decodes the injected JSON rows. Real temporary folders
    settle normally before the injected cleanup OSError. Simulated platform and
    hosted-environment inputs establish no native/CI/queue capability.
    """
    import errno
    import sys
    import tempfile
    from unittest import mock
    specification = importlib.util.spec_from_file_location("collector_control", driver_path)
    collector = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(collector)
    real_temporary = tempfile.TemporaryDirectory
    real_specification = importlib.util.spec_from_file_location
    events = []
    with real_temporary(prefix="n3-synthetic-collector-") as controlled:
        controlled = Path(controlled)
        sdk = controlled / "synthetic-sdk"
        for relative in ("bsm/libbsm.h", "sys/appleapiopts.h", "mach/message.h",
                         "mach/mach_traps.h", "sys/un.h", "sys/syscall.h", "unistd.h"):
            path = sdk / "usr/include" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* Synthetic capture input; no API declarations. */\n", encoding="utf-8")
        destination = controlled / "output/result.json"

        def artificial_execute(command, timeout):
            command = list(command)
            events.append({"attempted_command": command, "actually_executed": False})
            row = {"command": command, "returncode": 0, "timed_out": False,
                   "cleanup_error": None, "stdout": "", "stderr": ""}
            if command == ["xcrun", "--show-sdk-path"]:
                row["stdout"] = str(sdk)
            elif len(command) == 2 and command[1] in ("fork", "raw_fork"):
                case = command[1]
                if fault == "raw_runtime_exception" and case == "raw_fork":
                    raise RuntimeError("synthetic raw command exception")
                artificial = copy.deepcopy(positive)
                artificial["records"][0]["case"] = case
                row["stdout"] = "\n".join(json.dumps(item) for item in artificial["records"])
                events.append({"synthetic_case_receipt": case})
            return row

        def controlled_specification(name, path):
            spec = real_specification(name, path)
            if name == "retained_native_driver":
                real_load = spec.loader.exec_module
                def controlled_load(dependency):
                    real_load(dependency)
                    dependency.execute = artificial_execute
                spec.loader.exec_module = controlled_load
            return spec

        class ControlledTemporary:
            def __init__(self, *args, **kwargs):
                self.owned = real_temporary(*args, **kwargs)
            def __enter__(self):
                return self.owned.__enter__()
            def __exit__(self, kind, value, traceback):
                returned = self.owned.__exit__(kind, value, traceback)
                events.append({"actual_temporary_folder_settled": True})
                if fault == "cleanup_after_cases" and kind is None:
                    events.append({"injected_cleanup_fault": "OSError EIO; not an actual filesystem failure"})
                    raise OSError(errno.EIO, "synthetic collector cleanup after cases")
                return returned

        argv = [str(driver_path), "--hosted-ci", "--expected-arch", "arm64",
                "--output", str(destination), "--shared-driver",
                str(shared_driver)]
        with mock.patch.object(collector.platform, "system", return_value="Darwin"), \
             mock.patch.object(collector.platform, "machine", return_value="arm64"), \
             mock.patch.object(collector.platform, "release", return_value="synthetic"), \
             mock.patch.object(collector.platform, "version", return_value="synthetic"), \
             mock.patch.object(collector.shutil, "which", return_value="synthetic-no-executable"), \
             mock.patch.dict(collector.os.environ, {"GITHUB_ACTIONS": "true"}), \
             mock.patch.object(collector.importlib.util, "spec_from_file_location", controlled_specification), \
             mock.patch.object(collector.tempfile, "TemporaryDirectory", ControlledTemporary), \
             mock.patch.object(sys, "argv", argv):
            returned = collector.main()
        result = json.loads(destination.read_text(encoding="utf-8"))
    return {"evidence_kind": "actual Python collector with synthetic platform/command/runtime/fault inputs",
            "collector_returncode": returned, "result": result, "events": events,
            "actual_native_api_or_compiler_executed": False,
            "driver_sha256": hashlib.sha256(driver_path.read_bytes()).hexdigest()}


collector_controls = []
for name, fault, expected_return, expected_complete, expected_cases in (
        ("collector_settled_success_synthetic", None, 0, True, {"fork", "raw_fork"}),
        ("collector_cleanup_fault_preserves_both_receipts_rejects_success", "cleanup_after_cases", 1, False, {"fork", "raw_fork"}),
        ("collector_midcollection_exception_preserves_partial_receipt", "raw_runtime_exception", 1, False, {"fork"})):
    observed = collector_flow(driver, shared, fault)
    collected = observed["result"]
    passed = (observed["collector_returncode"] == expected_return
              and collected["native_capability_gate_completed"] is expected_complete
              and collected["status"] == ("native_capability_observed" if expected_complete else "native_gate_not_completed")
              and set(collected["cases"]) == expected_cases
              and collected["standard_header_compile_and_link"] is True
              and collected["raw_fork_diagnostic_compile_and_link"] is True
              and collected["n3_acceptance"] is False
              and collected["public_sdk_support_conclusion"] == "unproved")
    if fault:
        passed = passed and bool(collected.get("gate_error"))
    else:
        passed = passed and "gate_error" not in collected
    if fault == "cleanup_after_cases":
        passed = passed and len(collected["commands"]) == 12 and "synthetic collector cleanup after cases" in collected["gate_error"]
    collector_controls.append({"case": name, "passed": passed, "observed": observed})
    outcomes.append({"case": name, "expected": True, "actual": passed, "passed": passed})
historical = None
if args.historical_driver:
    historical = collector_flow(Path(args.historical_driver).resolve(), shared, "cleanup_after_cases")
    prior_result = historical["result"]
    assert historical["collector_returncode"] == 0
    assert prior_result["native_capability_gate_completed"] is True
    assert prior_result["status"] == "native_capability_observed" and bool(prior_result["gate_error"])
    assert set(prior_result["cases"]) == {"fork", "raw_fork"} and len(prior_result["commands"]) == 12
    historical["historical_false_success_reproduced"] = True

result = {"evidence_kind": "source-derived synthetic classifier, actual Python collector under synthetic inputs, and syntax controls",
          "native_runtime_executed": False, "n3_acceptance": False,
          "shared_driver": {"path": str(shared),
                            "sha256": hashlib.sha256(shared.read_bytes()).hexdigest()},
          "passed": sum(row["passed"] for row in outcomes), "total": len(outcomes),
          "outcomes": outcomes,
          "synthetic_collector_controls": collector_controls,
          "historical_negative_control": historical,
          "source_hashes": {name: hashlib.sha256((source/name).read_bytes()).hexdigest()
                            for name in ("darwin_audit_probe.c", "run_darwin_audit_probe.py",
                                         "validate_driver_contract.py")}}
destination.write_text(json.dumps(result, indent=2), encoding="utf-8")
raise SystemExit(0 if all(row["passed"] for row in outcomes) else 1)
