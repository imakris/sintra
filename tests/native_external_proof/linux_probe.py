#!/usr/bin/env python3
"""Isolated native Linux connection-pidfd probe. No Sintra production code."""
import ctypes
import json
import os
import platform
import select
import signal
import socket
import struct
import tempfile
import time
import traceback

FRAME_LIMIT = 4096
STEP_SECONDS = 5

def failure(error):
    return {"type":type(error).__name__, "errno":getattr(error, "errno", None),
            "message":str(error), "traceback":traceback.format_exc()}

def emit(record):
    # The outer driver retains this stream even if the fixture is terminated.
    print(json.dumps(record, sort_keys=True), flush=True)

def checkpoint(result, stage):
    # Only the final top-level report can advertise completed observations.
    emit({"probe":result["probe"], "stage":stage, "snapshot":result})

def recv_frame(conn):
    deadline = time.monotonic() + STEP_SECONDS
    data = bytearray()
    credentials = []
    while b"\n" not in data:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("bounded startup-frame deadline")
        conn.settimeout(remaining)
        block, ancillary, flags, _ = conn.recvmsg(FRAME_LIMIT, socket.CMSG_SPACE(12))
        emit({"probe":"linux_so_peerpidfd_v2", "stage":"received_frame_bytes",
              "block_hex":block.hex(), "flags":flags,
              "ancillary":[{"level":level, "kind":kind, "raw_hex":raw.hex()}
                           for level, kind, raw in ancillary]})
        if not block:
            raise RuntimeError("EOF before complete startup frame")
        if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
            raise RuntimeError("truncated frame or native credentials")
        data.extend(block)
        if len(data) > FRAME_LIMIT:
            raise RuntimeError("startup frame exceeded bound")
        for level, kind, raw in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS:
                if len(raw) != 12:
                    raise RuntimeError("unexpected native credential size")
                credentials.append(struct.unpack("3i", raw))
    frame, suffix = bytes(data).split(b"\n", 1)
    if suffix:
        raise RuntimeError("unexpected pipelined startup frame")
    return json.loads(frame), credentials

def send_frame(conn, value):
    conn.settimeout(STEP_SECONDS)
    conn.sendall(json.dumps(value, separators=(",", ":")).encode() + b"\n")

def receive_command(conn, expected):
    conn.settimeout(STEP_SECONDS)
    deadline = time.monotonic() + STEP_SECONDS
    received = bytearray()
    while b"\n" not in received:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("bounded command deadline")
        conn.settimeout(remaining)
        block = conn.recv(64)
        if not block:
            raise RuntimeError("EOF while awaiting command")
        received.extend(block)
        if len(received) > 64:
            raise RuntimeError("command exceeded bound")
    if bytes(received) != expected:
        raise RuntimeError("unexpected startup command")

def ready_pidfd(fd, seconds):
    poller = select.poll()
    poller.register(fd, select.POLLIN | select.POLLHUP | select.POLLERR)
    return poller.poll(round(seconds * 1000))

def child_main(address):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(STEP_SECONDS)
    client.connect(address)
    original_pid = os.getpid()
    send_frame(client, {"stage":"ARM", "actual_pid":original_pid,
                        "cached_claim_pid":original_pid})
    receive_command(client, b"READY\n")
    descendant = os.fork()
    if descendant:
        send_frame(client, {"stage":"FORKED", "actual_pid":original_pid,
                            "descendant_pid":descendant})
        os._exit(0)
    # Inherit the original connection, then await original native exit.
    receive_command(client, b"CONTINUE\n")
    send_frame(client, {"stage":"CONFIRM", "actual_pid":os.getpid(),
                        "cached_claim_pid":original_pid})
    client.close()
    os._exit(0)

def owned_children():
    # Only this single-threaded fixture's unreaped children; never name/PID search.
    with open("/proc/self/task/" + str(os.getpid()) + "/children", encoding="ascii") as stream:
        return [int(pid) for pid in stream.read().split()]

def settle_children(cleanup):
    deadline = time.monotonic() + STEP_SECONDS
    grace_deadline = time.monotonic() + 0.25
    consumed = cleanup.setdefault("consuming_waits", [])
    cleanup.setdefault("kill_attempts", [])
    cleanup.setdefault("killed_owned_pids", [])
    killed = set()
    while True:
        children = owned_children()
        if time.monotonic() >= grace_deadline:
            for pid in children:
                if pid not in killed:
                    # It is still our unreaped child: its PID cannot be reused.
                    try:
                        os.kill(pid, signal.SIGKILL)
                        killed.add(pid)
                        cleanup["killed_owned_pids"] = sorted(killed)
                        cleanup["kill_attempts"].append({"pid":pid, "return":0})
                    except ProcessLookupError as error:
                        cleanup["kill_attempts"].append({"pid":pid, "errno":error.errno,
                                                        "message":str(error)})
                    emit({"probe":"linux_so_peerpidfd_v2", "stage":"owned_cleanup", "cleanup":cleanup})
        while True:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                cleanup["completed"] = True
                return cleanup
            if not pid:
                break
            consumed.append({"pid":pid, "raw_status":status})
            emit({"probe":"linux_so_peerpidfd_v2", "stage":"owned_consuming_wait", "cleanup":cleanup})
        if time.monotonic() >= deadline:
            raise RuntimeError("owned child cleanup deadline; remaining=" + str(owned_children()))
        time.sleep(0.01)

def observe(result):
    if platform.system() != "Linux":
        result.update(status="unavailable", reason="requires native Linux")
        return result
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                          ctypes.c_ulong, ctypes.c_ulong]
    libc.prctl.restype = ctypes.c_int
    ctypes.set_errno(0)
    subreaper = libc.prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER; this process only.
    result["subreaper"] = {"return":subreaper,
                           "errno":ctypes.get_errno() if subreaper else 0}
    checkpoint(result, "subreaper_returned")
    if subreaper:
        raise RuntimeError("fixture cannot own/reap its descendant")
    connection = None
    pidfd = None
    with tempfile.TemporaryDirectory(prefix="sintra-native-pidfd-") as directory:
        address = directory + "/control.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        listener.bind(address)
        listener.listen(1)
        listener.settimeout(STEP_SECONDS)
        try:
            original = os.fork()
            if not original:
                listener.close()
                try:
                    child_main(address)
                except BaseException as error:
                    try:
                        emit({"probe":result["probe"], "stage":"child_exception",
                              "actual_pid":os.getpid(), "failure":failure(error)})
                    finally:
                        os._exit(33)
            result["owned_original_pid"] = original
            checkpoint(result, "owned_original_created")
            connection, _ = listener.accept()
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            arm, arm_creds = recv_frame(connection)
            peer = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET,
                                                            socket.SO_PEERCRED, 12))
            result.update(arm=arm, arm_credentials=arm_creds, connection_peer=peer)
            checkpoint(result, "arm_observed")
            if arm["stage"] != "ARM" or arm["actual_pid"] != original or peer[0] != original:
                raise RuntimeError("native ARM connection mismatch")
            if not arm_creds or any(credential[0] != original for credential in arm_creds):
                raise RuntimeError("ARM credentials did not identify original")
            # Linux v6.8 include/uapi/asm-generic/socket.h defines SO_PEERPIDFD=77.
            option = getattr(socket, "SO_PEERPIDFD", 77)
            result["option"] = option
            try:
                pidfd = connection.getsockopt(socket.SOL_SOCKET, option)
            except OSError as error:
                result.update(status="unavailable",
                              native_error={"errno":error.errno, "message":str(error)})
                checkpoint(result, "pidfd_unavailable")
                return result
            result["pidfd_alive_events"] = ready_pidfd(pidfd, 0)
            checkpoint(result, "pidfd_alive_observed")
            if result["pidfd_alive_events"]:
                raise RuntimeError("original already terminal before READY")
            with open("/proc/self/fdinfo/" + str(pidfd), encoding="ascii") as stream:
                result["pidfd_fdinfo_before"] = stream.read()
            checkpoint(result, "pidfd_fdinfo_before_observed")
            connection.sendall(b"READY\n")
            forked, fork_creds = recv_frame(connection)
            result.update(forked=forked, fork_credentials=fork_creds)
            checkpoint(result, "forked_observed")
            if forked["stage"] != "FORKED" or forked["actual_pid"] != original:
                raise RuntimeError("original did not announce descendant")
            events = ready_pidfd(pidfd, STEP_SECONDS)
            result["original_exit_native_events"] = events
            checkpoint(result, "original_exit_native_observed")
            if not any(mask & select.POLLIN for _, mask in events):
                raise RuntimeError("no native exit readiness; timeout is not death")
            # Preserve original child/PID until all protocol observations finish.
            status = os.waitid(os.P_PID, original,
                               os.WEXITED | os.WNOWAIT | os.WNOHANG)
            result["original_nonconsuming_status"] = None if status is None else {
                "pid":status.si_pid, "uid":status.si_uid, "signo":status.si_signo,
                "code":status.si_code, "status":status.si_status}
            checkpoint(result, "original_nonconsuming_status_observed")
            if status is None or status.si_code != os.CLD_EXITED or status.si_status != 0:
                raise RuntimeError("original nonconsuming child status was not normal exit")
            connection.sendall(b"CONTINUE\n")
            confirm, confirm_creds = recv_frame(connection)
            result.update(confirm=confirm, confirm_credentials=confirm_creds)
            checkpoint(result, "confirm_observed")
            if (confirm["stage"] != "CONFIRM" or confirm["cached_claim_pid"] != original
                    or confirm["actual_pid"] != forked["descendant_pid"]):
                raise RuntimeError("inherited-channel case was not established")
            if not confirm_creds or any(c[0] != confirm["actual_pid"] for c in confirm_creds):
                raise RuntimeError("CONFIRM native credentials did not identify descendant")
            retained = ready_pidfd(pidfd, 0)
            result["retained_original_events_after_confirm"] = retained
            checkpoint(result, "retained_original_observed")
            if not any(mask & select.POLLIN for _, mask in retained):
                raise RuntimeError("retained original authority lost its exit fact")
            try:
                reacquired = connection.getsockopt(socket.SOL_SOCKET, option)
            except OSError as error:
                result["reacquire_peer_pidfd"] = {"errno":error.errno, "message":str(error)}
            else:
                try:
                    result["reacquire_peer_pidfd"] = {
                        "returned":True, "native_events":ready_pidfd(reacquired, 0)}
                finally:
                    os.close(reacquired)
            checkpoint(result, "reacquire_peer_pidfd_observed")
            with open("/proc/self/fdinfo/" + str(pidfd), encoding="ascii") as stream:
                result["pidfd_fdinfo_after"] = stream.read()
            checkpoint(result, "pidfd_fdinfo_after_observed")
            result.update(status="supported_observed",
                          observation="descendant CONFIRM; original native authority stays terminal")
            return result
        except Exception as error:
            result["failure"] = failure(error)
            checkpoint(result, "parent_exception")
            raise
        finally:
            result["cleanup"] = {"completed":False}
            actions = [("listener_close", listener.close)]
            if connection is not None:
                actions.insert(0, ("connection_close", connection.close))
            if pidfd is not None:
                actions.append(("pidfd_close", lambda: os.close(pidfd)))
            actions.append(("owned_children", lambda: settle_children(result["cleanup"])))
            for name, action in actions:
                try:
                    action()
                except Exception as error:
                    result.setdefault("cleanup_failures", []).append({"operation":name, **failure(error)})
            if result.get("cleanup_failures"):
                result["status"] = "probe_error"
            checkpoint(result, "cleanup_settled_or_failed")

def main():
    result = {"probe":"linux_so_peerpidfd_v2", "status":"unexecuted",
              "runtime":{"kernel":platform.release(), "machine":platform.machine(),
                         "python":platform.python_version(), "system":platform.system()},
              "pid_reuse_exercised":False, "sintra_process_word_exercised":False,
              "scope":"connection authority, original exit, inherited channel; no N3 acceptance"}
    try:
        observe(result)
    except Exception as error:
        result.setdefault("failure", failure(error))
        result["status"] = "probe_error"
    return result

if __name__ == "__main__":
    record = main()
    emit(record)
    raise SystemExit(0 if record["status"] == "supported_observed"
                     else 1 if record["status"] == "probe_error" else 2)
