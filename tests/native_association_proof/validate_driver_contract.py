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
args = parser.parse_args()
source = Path(__file__).resolve().parent
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
result = {"evidence_kind": "source-derived synthetic classifier faults and Python syntax only",
          "native_runtime_executed": False, "n3_acceptance": False,
          "passed": sum(row["passed"] for row in outcomes), "total": len(outcomes),
          "outcomes": outcomes,
          "source_hashes": {name: hashlib.sha256((source/name).read_bytes()).hexdigest()
                            for name in ("darwin_audit_probe.c", "run_darwin_audit_probe.py",
                                         "validate_driver_contract.py")}}
destination.write_text(json.dumps(result, indent=2), encoding="utf-8")
raise SystemExit(0 if all(row["passed"] for row in outcomes) else 1)
