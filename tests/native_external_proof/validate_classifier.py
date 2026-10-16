#!/usr/bin/env python3
"""Causal classifier regressions; synthetic transcripts, no native capability claim."""
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

source = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("native_probe_driver", source/"run_native_probes.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)
outcomes = []
# Literal stage names are from immutable Darwin fixture observations, independent
# of the classifier table; deriving oracle stages from the table could hide drift.
ORACLE_STAGES = {
    "dup": ("attempt_guarded_dup", "guarded_dup_returned"),
    "send_rights": ("attempt_guarded_scm_rights", "guarded_scm_rights_returned"),
    "fileport": ("attempt_guarded_fileport", "guarded_fileport_returned"),
}
if driver._DARWIN_NEGATIVE_STAGES != ORACLE_STAGES:
    raise AssertionError("classifier stage contract differs from literal fixture oracle")


def check(name, case, transcript, expected, completed=False, violation=False):
    actual = driver.classify_darwin_negative(case, transcript)
    wanted = (expected, completed, violation)
    observed = (actual["classification"], actual["gate_completed"], actual["observed_violation"])
    outcomes.append({"name": name, "case": case, "input": transcript, "expected": wanted,
                     "actual": actual, "passed": observed == wanted})

def sample(case, value=None, error=None, code=0, timed_out=False, cleanup_error=None,
           attempt=True):
    attempted, returned = ORACLE_STAGES[case]
    rows = [{"stage": attempted, "return": 7, "errno": 0}] if attempt else []
    if value is not None:
        rows.append({"stage": returned, "return": value, "errno": error})
    return {"records": rows, "returncode": code, "timed_out": timed_out,
            "cleanup_error": cleanup_error}

for case in ("dup", "send_rights", "fileport"):
    success = 0  # fd0/send0/port0 are boundary successful observations.
    for code in (0, 23, -5, -9):
        check("success_dominates_exit_" + str(code), case, sample(case, success, 0, code),
              "returned_success_violation", violation=True)
    check("success_dominates_timeout", case, sample(case, success, 0, -9, True),
          "returned_success_violation", violation=True)
    check("success_dominates_cleanup_failure", case,
          sample(case, success, 0, -9, cleanup_error="unsettled"),
          "returned_success_violation", violation=True)
    check("success_without_attempt_still_violation", case,
          sample(case, success, 0, -5, attempt=False),
          "returned_success_violation", violation=True)
    check("actual_returned_rejection", case, sample(case, -1, 1),
          "returned_rejection", completed=True)
    check("rejection_then_signal_is_distinct", case, sample(case, -1, 1, -5),
          "returned_rejection_then_termination_unclassified")
    check("attempt_then_signal_is_not_rejection", case, sample(case, code=-5),
          "terminated_after_attempt_unclassified")
    check("unavailable_is_not_rejection", case, sample(case, code=20, attempt=False),
          "setup_unavailable")
    check("setup_failure_is_not_rejection", case, sample(case, code=21, attempt=False),
          "setup_failure")
    check("no_return_on_normal_exit_is_incomplete", case, sample(case),
          "missing_operation_return")
    check("timeout_is_not_native_rejection", case, sample(case, -1, 1, -9, True),
          "diagnostic_timeout")
    check("reject_with_cleanup_failure_is_incomplete", case,
          sample(case, -1, 1, 0, cleanup_error="unsettled"), "fixture_cleanup_unsettled")
    check("missing_errno_is_not_validated_rejection", case, sample(case, -1, None),
          "operation_return_unrecognized")
    check("boolean_return_is_not_native_integer", case, sample(case, True, 0),
          "operation_return_unrecognized")
    transcript = sample(case, -1, 1, -5)
    transcript["records"].append({"stage": ORACLE_STAGES[case][1],
                                  "return": success, "errno": 0})
    check("later_success_dominates_earlier_rejection", case, transcript,
          "returned_success_violation", violation=True)
    transcript = sample(case, -1, 1)
    transcript["records"].reverse()
    check("reversed_order_is_incomplete", case, transcript, "operation_order_unresolved")

# Positive-valued fileport result is neither native success0 nor validated failure-1.
check("fileport_unknown_positive_return", "fileport", sample("fileport", 1, 0),
      "operation_return_unrecognized")
# Exercise the actual JSON-line parser before classification on the B1 trace.
parsed = driver.records({"stdout": 'noise\n{"stage":"attempt_guarded_dup","return":7,"errno":0}\n'
                                  '{"stage":"guarded_dup_returned","return":8,"errno":0}\n'})
check("B1_raw_json_then_later_signal", "dup",
      {"records": parsed, "returncode": -5, "timed_out": False},
      "returned_success_violation", violation=True)
passed = all(row["passed"] for row in outcomes)
report = {"at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "validation": "synthetic causal classifier transcripts only",
          "material_oracle_source": "sibling B1 review SHA5DDC8648264B166530307FE508C62EDA0D5A7AB74F1A0B20C8945D3A8E6E249E",
          "driver_sha256": hashlib.sha256((source/"run_native_probes.py").read_bytes()).hexdigest(),
          "cases": outcomes, "passed": passed, "case_count": len(outcomes),
          "native_execution": False, "n3_acceptance": False}
destination = Path(sys.argv[1])
with destination.open("x", encoding="utf-8") as output:
    json.dump(report, output, indent=2)
print(json.dumps({"passed": passed, "case_count": len(outcomes), "report": str(destination)}))
sys.exit(0 if passed else 1)
