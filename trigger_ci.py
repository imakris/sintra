#!/usr/bin/env python3
"""
Trigger CI by flipping the value in flip_to_trigger_ci file.
Automatically commits and pushes the change.
"""

import subprocess
import sys
from pathlib import Path


def pause():
    """Pause and wait for user input before exiting."""
    try:
        input("\nPress Enter to continue...")
    except (KeyboardInterrupt, EOFError):
        print()


def run_command(cmd, check=True):
    """Run a shell command and return the result."""
    result = subprocess.run(
        cmd,
        shell=True,
        capture_output=True,
        text=True,
        check=False
    )
    if check and result.returncode != 0:
        print(f"Error running command: {cmd}", file=sys.stderr)
        print(f"stdout: {result.stdout}", file=sys.stderr)
        print(f"stderr: {result.stderr}", file=sys.stderr)
        pause()
        sys.exit(1)
    return result


def main():
    # Path to the flip file
    flip_file = Path(__file__).parent / "flip_to_trigger_ci"
    tests_trigger = Path(__file__).parent / "tests" / ".ci_trigger"

    # Read current value
    if flip_file.exists():
        current_value = flip_file.read_text().strip()
    else:
        current_value = "0"

    # Flip the value
    new_value = "1" if current_value == "0" else "0"

    print(f"Flipping CI trigger: {current_value} -> {new_value}")

    # Write new value
    flip_file.write_text(new_value)
    tests_trigger.parent.mkdir(parents=True, exist_ok=True)
    tests_trigger.write_text(new_value)

    # Also touch a workflows file to trigger the changed filter
    # This ensures CI actually runs tests instead of skipping them
    workflows_trigger = Path(__file__).parent / ".github" / "workflows" / ".ci_trigger"
    workflows_trigger.parent.mkdir(parents=True, exist_ok=True)
    workflows_trigger.write_text(new_value)

    # Git operations
    print("Adding files to git...")
    run_command("git add flip_to_trigger_ci tests/.ci_trigger .github/workflows/.ci_trigger")

    print("Creating commit...")
    commit_message = f"""Trigger CI (flip: {current_value} -> {new_value})

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude <noreply@anthropic.com>"""

    # Use subprocess.run with list arguments for proper handling
    result = subprocess.run(
        ["git", "commit", "-m", commit_message],
        capture_output=True,
        text=True,
        check=False
    )

    if result.returncode != 0:
        if "nothing to commit" in result.stdout or "nothing to commit" in result.stderr:
            print("No changes to commit. File may already be staged with this value.")
            pause()
            sys.exit(0)
        else:
            print(f"Commit failed!", file=sys.stderr)
            print(f"Return code: {result.returncode}", file=sys.stderr)
            print(f"stdout: {result.stdout}", file=sys.stderr)
            print(f"stderr: {result.stderr}", file=sys.stderr)
            pause()
            sys.exit(1)

    print("Pushing to remote...")
    run_command("git push")

    print("CI triggered successfully!")
    pause()


if __name__ == "__main__":
    main()
