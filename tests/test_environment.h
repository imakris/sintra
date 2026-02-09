#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <limits>
#include <thread>
#include <cerrno>

#ifdef _MSC_VER
#include <intrin.h>
#include <crtdbg.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <DbgHelp.h>
#include <process.h>
#endif

#ifndef _WIN32
#include <execinfo.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace sintra::test {

/// Trigger an immediate, deterministic crash.
/// On macOS abort; elsewhere use an illegal instruction.
/// This avoids UB while ensuring a crash state suitable for stack capture.
[[noreturn]] inline void trigger_illegal_instruction_crash()
{
#if defined(__APPLE__)
    std::abort();
#elif defined(_MSC_VER)
    __ud2();   // raises illegal-instruction on MSVC
#else
    __builtin_trap();   // raises illegal-instruction on GCC/Clang
#endif
}

inline void emit_self_stack_trace()
{
    std::fprintf(stderr, "[SINTRA_SELF_STACK_BEGIN]\n");
#ifdef _WIN32
#ifdef _MSC_VER
#pragma comment(lib, "Dbghelp.lib")
#endif
    void* stack[64] = {};
    const USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    const std::size_t symbol_size = sizeof(SYMBOL_INFO) + 256;
    auto* symbol = static_cast<SYMBOL_INFO*>(std::calloc(1, symbol_size));
    if (symbol) {
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
    }

    for (USHORT i = 0; i < frames; ++i) {
        const DWORD64 address = reinterpret_cast<DWORD64>(stack[i]);
        if (symbol && SymFromAddr(process, address, nullptr, symbol)) {
            std::fprintf(
                stderr,
                "%u: %s - 0x%llx\n",
                static_cast<unsigned>(i),
                symbol->Name,
                static_cast<unsigned long long>(symbol->Address));
        }
        else {
            std::fprintf(
                stderr,
                "%u: 0x%llx\n",
                static_cast<unsigned>(i),
                static_cast<unsigned long long>(address));
        }
    }

    if (symbol) {
        std::free(symbol);
    }
#else
    void* stack[64] = {};
    const int frames = ::backtrace(stack, 64);
    ::backtrace_symbols_fd(stack, frames, STDERR_FILENO);
#endif
    std::fprintf(stderr, "[SINTRA_SELF_STACK_END]\n");
    std::fflush(stderr);
}

/// Trigger a crash for stack capture probes, with fallbacks if the first crash
/// path is ignored.
[[noreturn]] inline void trigger_stack_capture_crash(const char* reason)
{
    const char* label = reason ? reason : "stack-capture";
    std::fprintf(stderr, "[SINTRA_STACK_CAPTURE] Triggering crash: %s\n", label);
    std::fflush(stderr);

#if defined(__APPLE__)
    std::signal(SIGABRT, SIG_DFL);
    std::signal(SIGILL, SIG_DFL);
    std::signal(SIGSEGV, SIG_DFL);
    std::raise(SIGABRT);
    std::raise(SIGILL);
    std::raise(SIGSEGV);
    __builtin_trap();
#elif defined(_MSC_VER)
    __ud2();   // raises illegal-instruction on MSVC
#else
    __builtin_trap();   // raises illegal-instruction on GCC/Clang
#endif

    std::fprintf(stderr,
                 "[SINTRA_STACK_CAPTURE] ERROR: crash did not terminate process\n");
    std::fflush(stderr);
    std::_Exit(127);
}

inline int read_env_int(const char* name, int default_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return default_value;
    }

    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (!end || end == value) {
        return default_value;
    }
    if (parsed > static_cast<long>(std::numeric_limits<int>::max())) {
        return default_value;
    }
    if (parsed < static_cast<long>(std::numeric_limits<int>::min())) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

inline int get_pid()
{
#ifdef _WIN32
    return _getpid();
#else
    return ::getpid();
#endif
}

inline void prepare_for_intentional_crash(const char* context = nullptr)
{
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
    struct rlimit current {};
    if (getrlimit(RLIMIT_CORE, &current) != 0) {
        if (context) {
            std::fprintf(stderr,
                         "[%s] getrlimit(RLIMIT_CORE) failed: %d\n",
                         context,
                         errno);
        }
        return;
    }

    if (current.rlim_cur == 0) {
        return;
    }

    struct rlimit updated = current;
    updated.rlim_cur = 0;
    if (setrlimit(RLIMIT_CORE, &updated) != 0) {
        if (context) {
            std::fprintf(stderr,
                         "[%s] setrlimit(RLIMIT_CORE) failed: %d\n",
                         context,
                         errno);
        }
    }
#endif
}

inline void precrash_pause(const char* reason)
{
    const int pause_ms = read_env_int("SINTRA_CRASH_CAPTURE_PAUSE_MS", 0);
    if (pause_ms <= 0) {
        return;
    }

    const auto pid = static_cast<unsigned long long>(get_pid());

    std::fprintf(stderr,
                 "[SINTRA_DEBUG_PAUSE] Process %llu paused: %s\n",
                 pid,
                 reason ? reason : "precrash");
    std::fprintf(stderr,
                 "[SINTRA_DEBUG_PAUSE] Attach debugger to PID %llu to capture stacks\n",
                 pid);
    std::fflush(stderr);

    std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
}

inline const std::filesystem::path& scratch_root()
{
    static const std::filesystem::path root = [] {
        const char* override_value = std::getenv("SINTRA_TEST_ROOT");
        if (override_value && *override_value) {
            std::filesystem::path override_path(override_value);
            std::filesystem::create_directories(override_path);
            return override_path;
        }

        auto fallback = std::filesystem::temp_directory_path() / "sintra_tests";
        std::filesystem::create_directories(fallback);
        return fallback;
    }();

    return root;
}

inline std::filesystem::path scratch_subdirectory(std::string_view name)
{
    auto directory = scratch_root() / std::filesystem::path(std::string(name));
    std::filesystem::create_directories(directory);
    return directory;
}

inline std::filesystem::path unique_scratch_directory(std::string_view prefix)
{
    static std::atomic<std::uint64_t> counter{0};

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    auto sequence = counter.fetch_add(1);

    std::ostringstream oss;
    oss << prefix << '_' << ticks << '_' << sequence;

    auto directory = scratch_subdirectory("runs") / oss.str();
    std::filesystem::create_directories(directory);
    return directory;
}

} // namespace sintra::test

