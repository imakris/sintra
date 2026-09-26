"""Discover CMake manual targets in single- and multi-configuration builds."""

from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tests import run_tests
from tests.debuggers.base import NullDebuggerStrategy
from tests.runner.platform.windows import WindowsPlatformSupport


class Runner_discovery_test(unittest.TestCase):
    def test_manual_targets_follow_cmake_output_directories(self):
        for multi_config in (False, True):
            with self.subTest(multi_config=multi_config), tempfile.TemporaryDirectory() as directory:
                build_dir = Path(directory)
                configs = ("Debug", "Release") if multi_config else ("Debug",)
                expected = {}
                for config in configs:
                    for subdir, target in (("", "utility_test"),
                                           ("manual", "manual_crash_capture_self_test")):
                        output_dir = build_dir / "tests" / subdir
                        if multi_config:
                            output_dir /= config
                        output_dir.mkdir(parents=True, exist_ok=True)
                        binary = output_dir / f"sintra_{target}.exe"
                        binary.touch()
                        expected[(config.lower(), target)] = binary

                with mock.patch.object(run_tests, "get_debugger_strategy", return_value=NullDebuggerStrategy(False)), \
                     mock.patch.object(run_tests, "get_platform_support", return_value=WindowsPlatformSupport()):
                    runner = run_tests.TestRunner(build_dir, configs, 5.0, False)
                suites, _ = runner.find_test_suites({
                    "utility_test": 1,
                    "manual/crash_capture_self_test": 1,
                })
                self.assertEqual(set(suites), {config.lower() for config in configs})
                for config, invocations in suites.items():
                    self.assertEqual({item.path for item in invocations}, {
                        expected[(config, "utility_test")],
                        expected[(config, "manual_crash_capture_self_test")],
                    })


if __name__ == "__main__":
    unittest.main()
