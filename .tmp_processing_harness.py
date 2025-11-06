import os
import subprocess
import sys
import time
from pathlib import Path

exe = Path('build-ninja/tests/sintra_processing_fence_test_debug.exe')
out_file = Path('processing_fence_manual_stdout.log')
if out_file.exists():
    out_file.unlink()
if not exe.exists():
    print(f'Executable not found: {exe}')
    sys.exit(1)

def run(exe: Path, timeout: int) -> int:
    print('=' * 70)
    print(f'Running {exe}')
    print('=' * 70)
    start = time.time()
    env = os.environ.copy()
    env['SINTRA_TRACE_BARRIER'] = '1'
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == 'win32' else 0
    proc = subprocess.Popen(
        [str(exe)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        creationflags=creationflags if sys.platform == 'win32' else 0,
        start_new_session=(sys.platform != 'win32')
    )
    try:
        stdout, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        duration = time.time() - start
        print(f'Timeout after {duration:.2f}s; terminating {proc.pid}')
        if sys.platform == 'win32':
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL,
                           timeout=5)
        else:
            proc.kill()
        stdout, _ = proc.communicate()
        ret = -1
    else:
        ret = proc.returncode
    duration = time.time() - start
    print(f'Exit code {ret} after {duration:.2f}s')
    out_file.write_text(stdout or '')
    print(f'Captured stdout -> {out_file}')
    return ret

rc = run(exe, 120)
print('Return code:', rc)
if rc != 0:
    sys.exit(rc if rc > 0 else 1)
