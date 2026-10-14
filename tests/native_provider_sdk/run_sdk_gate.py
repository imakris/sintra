#!/usr/bin/env python3
"""Inspect and link the adopted provider's public SDK surface, serially.

The caller owns the queued-build lease through this process's completion.
This gate does not execute the linked program or establish native semantics.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def uncomment(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def explicit_reply_definition(text, routine):
    """Add only a client argument and prefix; retain all routine slots/types."""
    # Public definitions can put #ifdef branches between `routine` and its
    # user-side name. Match that name's declaration line, retaining the branch.
    pattern = re.compile(
        r"^[ \t]*(?:routine\s+)?(?:PREFIX\s*\(\s*)?" + re.escape(routine)
        + r"\s*\)?\s*\(", re.I | re.M)
    matches = list(pattern.finditer(uncomment(text)))
    if len(matches) != 1:
        raise RuntimeError(f"expected one public definition for {routine}")
    # The source, including comments and conditionals, stays intact. Match the
    # declaration again rather than transplanting offsets from stripped text.
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(f"ambiguous SDK declaration for {routine}")
    position = matches[0].end()
    addition = "\n    ureplyport sintra_reply_port: sintra_reply_port_t;"
    prefix = (
        "userprefix sintra_darwin_;\n"
        "type sintra_reply_port_t = MACH_MSG_TYPE_MAKE_SEND_ONCE "
        "ctype: mach_port_t;\n")
    modified = text[:position] + addition + text[position:]
    modified = re.sub(r"(?im)^(\s*userprefix\s+)(\w+)(\s*;)",
                      r"\1sintra_darwin_\2\3", modified)
    return prefix + modified


def client_body(text, routine):
    pattern = re.compile(r"\bkern_return_t\s+(\w*" + re.escape(routine)
                         + r")\s*\(")
    matches = list(pattern.finditer(uncomment(text)))
    if len(matches) != 1:
        raise RuntimeError(f"expected one generated client body for {routine}")
    code = uncomment(text)
    match = matches[0]
    opening = code.index("{", match.end())
    depth = 1
    end = opening + 1
    while depth and end < len(code):
        depth += (code[end] == "{") - (code[end] == "}")
        end += 1
    if depth:
        raise RuntimeError("unbalanced generated client body")
    return match.group(1), code[opening:end]


def check_client(original, private, routine):
    _, baseline = client_body(original, routine)
    name, body = client_body(private, routine)
    identifiers = re.findall(r"\.msgh_id\s*=\s*(\d+)\s*;", baseline)
    actual = re.findall(r"\.msgh_id\s*=\s*(\d+)\s*;", body)
    if len(identifiers) != 1 or actual != identifiers:
        raise RuntimeError(f"generated wire ordinal differs for {routine}")
    if re.search(r"\bmig_(?:get|put|dealloc)\w*reply_port\s*\(", body):
        raise RuntimeError(f"inherited reply-port cache in {routine}")
    if not re.search(r"\.msgh_(?:local|reply)_port\s*=\s*sintra_reply_port\s*;", body):
        raise RuntimeError(f"explicit reply argument is not selected for {routine}")
    if not name.startswith("sintra_darwin_"):
        raise RuntimeError(f"private symbol prefix absent for {routine}")
    return {"symbol": name, "message_id": int(actual[0]), "reply_cache_calls": False}


class Sdk_gate:
    def __init__(self, output, architecture, sdk_version):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.architecture = architecture
        self.sdk_version = sdk_version
        self.cancel_signal = None
        self.result = {
            "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "expected_architecture": architecture,
            "requested_sdk": sdk_version,
            "commands": [],
            "files": [],
            "sdk_compile_link_passed": False,
            "native_provider_qualified": False,
            "native_image_import_proved": False,
            "linked_program_executed": False,
        }

    def request_cancel(self, signum, _frame):
        # No exception or I/O from signal delivery: command() owns settlement.
        self.cancel_signal = signum

    @staticmethod
    def wait_group_stopped(group):
        # Grandchildren are reparented to the OS, not waitpid children of this
        # owner. After killing the group, require every remaining member to be
        # a zombie (no executable work) or absent before releasing ownership.
        while True:
            try:
                listing = subprocess.run(["ps", "-axo", "pgid=,stat="], text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
                    check=True).stdout
                live = any(fields[0] == str(group) and not fields[1].startswith("Z")
                           for line in listing.splitlines()
                           if len(fields := line.split()) >= 2)
                if not live:
                    return
            except (OSError, subprocess.SubprocessError):
                # An inspection failure cannot release an owned compiler group.
                pass
            time.sleep(0.1)

    def command(self, arguments, timeout=90):
        if self.cancel_signal is not None:
            raise RuntimeError("SDK gate cancellation requested")
        arguments = [str(value) for value in arguments]
        index = len(self.result["commands"])
        process = subprocess.Popen(arguments, cwd=self.output, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
        timed_out = False
        failure = None
        deadline = time.monotonic() + timeout
        try:
            while self.cancel_signal is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    timed_out = True
                    break
                try:
                    process.communicate(timeout=min(remaining, 0.1))
                    break
                except subprocess.TimeoutExpired:
                    continue
        except BaseException as error:
            failure = error
        finally:
            # This command's session also owns compiler/MIG descendants. Every
            # path settles it, including cancellation and an unexpected error.
            # A successful direct child must not leave an orphan in its group.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            finally:
                stdout, stderr = process.communicate()
                self.wait_group_stopped(process.pid)
        for stream in ("stdout", "stderr"):
            (self.output / f"command_{index:02}_{stream}.txt").write_text(
                stdout if stream == "stdout" else stderr, encoding="utf-8")
        self.result["commands"].append({
            "arguments": arguments, "returncode": process.returncode,
            "timed_out": timed_out,
            "cancel_signal": self.cancel_signal,
            "direct_child_reaped": True,
            "no_live_process_group_members": True,
            "stdout": f"command_{index:02}_stdout.txt",
            "stderr": f"command_{index:02}_stderr.txt",
        })
        if failure is not None:
            raise failure
        if timed_out or self.cancel_signal is not None or process.returncode:
            raise RuntimeError(f"command {index} failed: {arguments[0]}")
        return stdout.strip()

    def retain(self, path, name):
        destination = self.output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
        self.result["files"].append({
            "source": str(path), "retained": name, "sha256": sha256(destination),
        })

    def run(self):
        if platform.system() != "Darwin" or platform.machine() != self.architecture:
            raise RuntimeError("gate requires the declared native Darwin architecture")
        self.result["uname"] = self.command(["uname", "-a"])
        self.result["sw_vers"] = self.command(["sw_vers"])
        version = self.command(["sw_vers", "-productVersion"])
        if version.split(".")[0] != "15":
            raise RuntimeError("this minimum-platform gate requires a macOS15 runtime")
        self.result["xcode"] = self.command(["xcodebuild", "-version"])
        self.result["developer_dir"] = os.environ.get("DEVELOPER_DIR")
        sdk_name = f"macosx{self.sdk_version}"
        xcrun = ["xcrun", "--sdk", sdk_name]
        sdk = Path(self.command(xcrun + ["--show-sdk-path"]))
        self.result["sdk_path"] = str(sdk)
        actual = self.command(xcrun + ["--show-sdk-version"])
        if actual != self.sdk_version:
            raise RuntimeError(f"requested SDK {self.sdk_version}, selected {actual}")
        self.result["sdk_version"] = actual
        self.result["clang"] = self.command(xcrun + ["--find", "clang"])
        self.result["mig"] = self.command(xcrun + ["--find", "mig"])
        self.result["compiler_version"] = self.command(xcrun + ["clang", "--version"])
        self.result["source_sha"] = os.environ.get("GITHUB_SHA")
        self.result["run_id"] = os.environ.get("GITHUB_RUN_ID")
        self.result["runner_image"] = os.environ.get("ImageOS")
        self.result["runner_image_version"] = os.environ.get("ImageVersion")
        self.retain(Path(__file__), "inputs/run_sdk_gate.py")
        self.retain(Path(__file__).with_name("sdk_surface.cpp.in"), "inputs/sdk_surface.cpp.in")
        include = sdk / "usr/include"
        for relative in (
            "mach/mach_port.defs", "mach/thread_act.defs", "mach/std_types.defs",
            "mach/mach_types.defs", "mach/mach_port.h", "mach/thread_act.h",
            "mach/mach_traps.h", "mach/thread_switch.h", "sys/fcntl.h", "fcntl.h",
            "sys/un.h", "sys/socket.h", "bsm/libbsm.h",
        ):
            self.retain(include / relative, f"sdk/{relative}")

        compiler_flags = ["-arch", self.architecture, "-isysroot", str(sdk),
                          "-mmacosx-version-min=15.0"]
        clients = {}
        objects = []
        for definition, routine in (("mach_port", "mach_port_get_context"),
                                    ("thread_act", "thread_abort")):
            text = (include / f"mach/{definition}.defs").read_text()
            modified = self.output / f"private_{definition}.defs"
            modified.write_text(explicit_reply_definition(text, routine), encoding="utf-8")
            for kind, source in (("baseline", include / f"mach/{definition}.defs"),
                                 ("private", modified)):
                stem = f"{kind}_{definition}"
                self.command(xcrun + ["mig", "-arch", self.architecture,
                    "-isysroot", str(sdk), "-I", str(include),
                    "-user", f"{stem}.c", "-header", f"{stem}.h",
                    "-server", "/dev/null", "-sheader", "/dev/null", source])
            clients[routine] = check_client(
                (self.output / f"baseline_{definition}.c").read_text(),
                (self.output / f"private_{definition}.c").read_text(), routine)
            target = f"private_{definition}.o"
            self.command(xcrun + ["clang"] + compiler_flags + ["-std=c11", "-O0",
                "-Wall", "-Werror=implicit-function-declaration", "-c",
                f"private_{definition}.c", "-o", target])
            objects.append(target)

        self.result["clients"] = clients
        template = Path(__file__).with_name("sdk_surface.cpp.in").read_text()
        for routine, details in clients.items():
            template = template.replace(f"@{routine}@", details["symbol"])
        (self.output / "sdk_surface.cpp").write_text(template, encoding="utf-8")
        self.command(xcrun + ["clang++"] + compiler_flags + ["-std=c++20", "-O0",
            "-Wall", "-Wextra", "-c", "sdk_surface.cpp", "-o", "sdk_surface.o"])
        self.command(xcrun + ["clang++"] + compiler_flags + [
            "sdk_surface.o", *objects, "-lbsm", "-o", "sdk_surface"])
        undefined = self.command(xcrun + ["nm", "-u", "sdk_surface.o"])
        if not re.search(r"\b_fcntl(?:\$INODE64)?\s*$", undefined, re.M):
            raise RuntimeError("ordinary public fcntl reference not found")
        if "NOCANCEL" in undefined.upper():
            raise RuntimeError("unexpected noncancelable fcntl alias")
        self.command(xcrun + ["otool", "-hv", "sdk_surface"])
        self.command(xcrun + ["otool", "-l", "sdk_surface"])
        self.result["sdk_compile_link_passed"] = True

    def settle(self):
        self.result["cancel_signal"] = self.cancel_signal
        if self.cancel_signal is not None:
            self.result["sdk_compile_link_passed"] = False
        self.result["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        self.result["output_hashes"] = [
            {"path": str(path.relative_to(self.output)), "sha256": sha256(path)}
            for path in sorted(self.output.rglob("*")) if path.is_file()
        ]
        (self.output / "result.json").write_text(
            json.dumps(self.result, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--architecture", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--sdk-version", default="15.5")
    args = parser.parse_args()
    gate = Sdk_gate(args.output, args.architecture, args.sdk_version)
    # The workflow execs this owner, so runner cancellation reaches it directly.
    # Install before any native command can be launched and retain through exit.
    signal.signal(signal.SIGINT, gate.request_cancel)
    signal.signal(signal.SIGTERM, gate.request_cancel)
    try:
        gate.run()
    except Exception as error:
        gate.result["error"] = f"{type(error).__name__}: {error}"
        print(gate.result["error"], file=sys.stderr)
    finally:
        gate.settle()
    return 0 if gate.result["sdk_compile_link_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
