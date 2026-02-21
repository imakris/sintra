# Release Notes

## v1.0.3 (2026-02-21)

### Highlights
- Fixed multiple lifecycle and synchronization races in IPC ring and message reader paths.
- Fixed RPC handler lifetime and exception cleanup behavior.
- Improved platform stability in CI/stress scenarios (including Windows and FreeBSD fixes).
- Expanded and hardened test coverage, including defensive and coverage-reporting updates.
- Refactored and deduplicated internal code paths to reduce complexity and improve maintainability.

### Compatibility
- No intended API break for existing consumers.
