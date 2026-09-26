"""Retain native process occurrences for cleanup of one Windows test."""

import ctypes
from ctypes import wintypes
import threading


class _Process_entry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


class _Process_api:
    def __init__(self):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "CreateToolhelp32Snapshot": ([wintypes.DWORD, wintypes.DWORD], wintypes.HANDLE),
            "Process32FirstW": ([wintypes.HANDLE, ctypes.POINTER(_Process_entry)], wintypes.BOOL),
            "Process32NextW": ([wintypes.HANDLE, ctypes.POINTER(_Process_entry)], wintypes.BOOL),
            "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            "GetCurrentProcess": ([], wintypes.HANDLE),
            "DuplicateHandle": ([wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE,
                                 ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD,
                                 wintypes.BOOL, wintypes.DWORD], wintypes.BOOL),
            "GetProcessTimes": ([wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4,
                                wintypes.BOOL),
            "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            "TerminateProcess": ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.kernel, name)
            function.argtypes = args
            function.restype = result

    def duplicate(self, handle):
        current = self.kernel.GetCurrentProcess()
        result = wintypes.HANDLE()
        if not self.kernel.DuplicateHandle(current, handle, current, ctypes.byref(result), 0, False, 2):
            raise ctypes.WinError(ctypes.get_last_error())
        return result.value

    def open(self, pid):
        # Synchronize, query limited information, and terminate this occurrence.
        return self.kernel.OpenProcess(0x00100000 | 0x1000 | 0x0001, False, pid)

    def times(self, handle):
        values = [wintypes.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(handle, *(ctypes.byref(value) for value in values)):
            raise ctypes.WinError(ctypes.get_last_error())
        return tuple((value.dwHighDateTime << 32) | value.dwLowDateTime for value in values[:2])

    def parents(self):
        snapshot = self.kernel.CreateToolhelp32Snapshot(2, 0)
        if snapshot == wintypes.HANDLE(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = _Process_entry()
            entry.dwSize = ctypes.sizeof(entry)
            result = {}
            valid = self.kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while valid:
                result[entry.th32ProcessID] = entry.th32ParentProcessID
                valid = self.kernel.Process32NextW(snapshot, ctypes.byref(entry))
            return result
        finally:
            self.close(snapshot)

    def running(self, handle):
        return self.kernel.WaitForSingleObject(handle, 0) == 258

    def terminate(self, handle):
        if self.running(handle) and not self.kernel.TerminateProcess(handle, 1):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self, handle):
        self.kernel.CloseHandle(handle)


class Windows_process_tree:
    def __init__(self, process):
        self._api = _Process_api()
        self._root_pid = process.pid
        # Popen retains the original native handle even if the child exits
        # before observation starts. Opening its PID would lose that identity.
        self._handles = {process.pid: self._api.duplicate(int(process._handle))}
        self._parents = {}
        self._lock = threading.RLock()

    def refresh(self):
        with self._lock:
            parents = self._api.parents()
            while True:
                acquired = False
                for pid, parent in parents.items():
                    if pid in self._handles or parent not in self._handles:
                        continue
                    handle = self._api.open(pid)
                    if not handle:
                        continue
                    try:
                        born, _ = self._api.times(handle)
                        parent_born, parent_exited = self._api.times(self._handles[parent])
                        # A reused parent PID cannot establish ownership, and
                        # a second snapshot rejects a reused candidate PID.
                        if (born < parent_born or (parent_exited and born > parent_exited)
                                or self._api.parents().get(pid) != parent):
                            continue
                        self._handles[pid] = handle
                        self._parents[pid] = parent
                        handle = None
                        acquired = True
                    finally:
                        if handle:
                            self._api.close(handle)
                if not acquired:
                    break

    def owns_live_pid(self, pid):
        with self._lock:
            handle = self._handles.get(pid)
            return handle is not None and self._api.running(handle)

    def live_descendants(self, root_pid=None):
        with self._lock:
            self.refresh()
            root_pid = self._root_pid if root_pid is None else root_pid
            result = []
            for pid, handle in self._handles.items():
                ancestor = self._parents.get(pid)
                while ancestor is not None and ancestor != root_pid:
                    ancestor = self._parents.get(ancestor)
                if ancestor == root_pid and self._api.running(handle):
                    result.append(pid)
            return result

    def terminate(self, *, include_root=True):
        with self._lock:
            self.refresh()
            for pid, handle in reversed(list(self._handles.items())):
                if include_root or pid != self._root_pid:
                    self._api.terminate(handle)

    def close(self):
        with self._lock:
            for handle in self._handles.values():
                self._api.close(handle)
            self._handles.clear()
