"""Runner isolation checks; no machine registry or unrelated process is changed."""

from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import os
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from unittest import mock

from tests import run_tests
from tests.debuggers.base import NullDebuggerStrategy
from tests.debuggers.windows import WindowsDebuggerStrategy
from tests.runner.platform.windows import WindowsPlatformSupport
from tests.runner.platform.windows_process_tree import Windows_process_tree


class Runner_safety_test(unittest.TestCase):
    def test_windows_cleanup_excludes_other_parent_occurrences(self):
        api = mock.Mock()
        api.duplicate.return_value = 1010
        api.parents.return_value = {10: 1, 20: 10, 30: 10, 40: 9}
        api.open.side_effect = lambda pid: pid + 1000
        api.times.side_effect = lambda handle: {
            1010: (100, 200), 1020: (150, 0), 1030: (250, 0)
        }[handle]
        api.running.side_effect = lambda handle: handle != 1010
        process = mock.Mock(pid=10, _handle=77)
        with mock.patch("tests.runner.platform.windows_process_tree._Process_api", return_value=api):
            tree = Windows_process_tree(process)
            self.assertEqual(tree.live_descendants(), [20])
            tree.terminate(include_root=False)
            tree.close()
        api.terminate.assert_called_once_with(1020)
        self.assertIn(mock.call(1030), api.close.call_args_list)
        api.open.assert_any_call(20)
        self.assertNotIn(mock.call(40), api.open.call_args_list)

    def test_windows_cleanup_rechecks_candidate_parent_after_open(self):
        api = mock.Mock()
        api.duplicate.return_value = 1010
        api.parents.side_effect = [{20: 10}, {20: 99}]
        api.open.return_value = 1020
        api.times.side_effect = [(150, 0), (100, 0)]
        process = mock.Mock(pid=10, _handle=77)
        with mock.patch("tests.runner.platform.windows_process_tree._Process_api", return_value=api):
            tree = Windows_process_tree(process)
            tree.refresh()
            tree.close()
        api.close.assert_any_call(1020)
        api.terminate.assert_not_called()

    @unittest.skipUnless(sys.platform == "win32", "Windows native process ownership")
    def test_concurrent_runs_of_same_executable_are_isolated(self):
        with tempfile.TemporaryDirectory() as directory:
            runners = [run_tests.TestRunner(Path(directory), "Debug", 5.0, False)
                       for _ in range(2)]
            invocation = run_tests.TestInvocation(
                Path(sys.executable), "shared_runner_probe",
                ("-c", "import time; time.sleep(0.5); print('child completed')"))
            with ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(lambda runner: runner.run_test_once(invocation), runners))
            for result in results:
                self.assertTrue(result.success, result.error)
                self.assertIn("child completed", result.output)

    @unittest.skipUnless(sys.platform == "win32", "Windows native process ownership")
    def test_owned_family_termination_leaves_same_name_sibling_running(self):
        sibling = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"])
        parent = subprocess.Popen(
            [sys.executable, "-c",
             "import subprocess,sys,time; "
             "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(10)']); "
             "print(child.pid,flush=True); time.sleep(10)"],
            stdout=subprocess.PIPE, text=True)
        tree = Windows_process_tree(parent)
        try:
            child_pid = int(parent.stdout.readline())
            self.assertIn(child_pid, tree.live_descendants())
            tree.terminate()
            parent.wait(timeout=5)
            self.assertEqual(tree.live_descendants(), [])
            self.assertIsNone(sibling.poll(), "a sibling with the same image name must survive")
        finally:
            tree.terminate()
            tree.close()
            parent.wait(timeout=5)
            parent.stdout.close()
            sibling.kill()
            sibling.wait(timeout=5)

    @unittest.skipUnless(sys.platform == "win32", "Windows native process ownership")
    def test_timeout_does_not_terminate_a_same_name_sibling(self):
        sibling = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"])
        try:
            with tempfile.TemporaryDirectory() as directory, \
                 mock.patch.object(run_tests, "get_debugger_strategy", return_value=NullDebuggerStrategy(False)):
                runner = run_tests.TestRunner(Path(directory), "Debug", 0.1, False)
                invocation = run_tests.TestInvocation(
                    Path(sys.executable), "timeout_runner_probe",
                    ("-c", "import time; time.sleep(10)"))
                result = runner.run_test_once(invocation)
                self.assertFalse(result.success)
                self.assertIn("TIMEOUT", result.error)
                self.assertIsNone(sibling.poll())
        finally:
            sibling.kill()
            sibling.wait(timeout=5)

    def test_core_cleanup_preserves_files_outside_invocation_scratch(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(run_tests, "get_debugger_strategy", return_value=NullDebuggerStrategy(False)):
            root = Path(directory)
            runner = run_tests.TestRunner(root, "Debug", 1.0, False)
            invocation = run_tests.TestInvocation(root / "probe.exe", "probe")
            scratch = runner._allocate_scratch_directory(invocation)
            owned_core = scratch / "core"
            other_core = root / "core.another_run"
            owned_core.write_text("owned dump")
            other_core.write_text("another run's dump")
            runner._cleanup_new_core_dumps(invocation, set(), 0, False, scratch)
            self.assertFalse(owned_core.exists())
            self.assertEqual(other_core.read_text(), "another run's dump")

    def test_startup_does_not_terminate_processes_it_did_not_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            platform = WindowsPlatformSupport()
            with mock.patch.object(run_tests, "get_platform_support", return_value=platform), \
                 mock.patch.object(run_tests, "get_debugger_strategy", return_value=NullDebuggerStrategy(False)), \
                 mock.patch("subprocess.run") as run:
                run_tests.TestRunner(Path(directory), "Debug", 2.0, False)
            termination_calls = [
                call for call in run.call_args_list
                if call.args and call.args[0][0] in ("taskkill", "pkill")
            ]
            self.assertEqual(termination_calls, [], "startup owns no process to terminate")

    def test_installed_debugger_is_used_without_a_download(self):
        with tempfile.TemporaryDirectory() as directory:
            debugger = Path(directory) / "cdb.exe"
            debugger.touch()
            with mock.patch.dict(os.environ, {"LOCALAPPDATA": directory}, clear=True), \
                 mock.patch("shutil.which", return_value=str(debugger)), \
                 mock.patch("urllib.request.urlopen", side_effect=urllib.error.URLError("network disabled in fixture")):
                path, error = WindowsDebuggerStrategy(False)._locate_windows_debugger("cdb")
            self.assertEqual(path, str(debugger))
            self.assertEqual(error, "")

    def test_windows_debugger_preparation_keeps_registry_read_only(self):
        registry = mock.MagicMock()
        registry.QueryValueEx.side_effect = FileNotFoundError
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.dict(sys.modules, {"winreg": registry}), \
             mock.patch.dict(os.environ, {"LOCALAPPDATA": directory}):
            strategy = WindowsDebuggerStrategy(False)
            strategy.ensure_crash_dumps()
            strategy._configured_dump_directories("probe.exe")
        registry.CreateKeyEx.assert_not_called()
        registry.SetValueEx.assert_not_called()


if __name__ == "__main__":
    unittest.main()
