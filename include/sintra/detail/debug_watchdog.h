/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef SINTRA_DETAIL_DEBUG_WATCHDOG_H
#define SINTRA_DETAIL_DEBUG_WATCHDOG_H

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifndef _WIN32
#  include <atomic>
#  include <csignal>
#  include <execinfo.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

namespace sintra::detail {

#ifndef _WIN32

inline void stack_trace_signal_handler(int sig, siginfo_t*, void*)
{
    const char intro[] = "\n[sintra] watchdog stack trace (SIGUSR2)\n";
    ::write(STDERR_FILENO, intro, sizeof(intro) - 1);

    void* frames[128]{};
    const int captured = ::backtrace(frames, 128);
    if (captured > 0) {
        ::backtrace_symbols_fd(frames, captured, STDERR_FILENO);
    }

    const char outro[] = "[sintra] end of stack trace\n";
    ::write(STDERR_FILENO, outro, sizeof(outro) - 1);

    (void)sig; // unused in release builds
}

struct Watchdog_state
{
    std::atomic<bool> installed{false};
    std::atomic<bool> handler_installed{false};
    pthread_t main_thread{};
};

inline Watchdog_state& watchdog_state()
{
    static Watchdog_state state;
    return state;
}

#endif // !_WIN32

inline void install_process_watchdog(std::chrono::seconds timeout, const char* context)
{
#ifndef _WIN32
    if (timeout <= std::chrono::seconds::zero()) {
        return;
    }

    auto& state = watchdog_state();
    bool expected = false;
    if (!state.installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    state.main_thread = pthread_self();

    expected = false;
    if (state.handler_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        struct sigaction action {};
        action.sa_sigaction = &stack_trace_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGUSR2, &action, nullptr);
    }

    const std::string label = context ? context : "watchdog";
    std::thread([timeout, label, thread_id = state.main_thread]() {
        std::this_thread::sleep_for(timeout);
        std::fprintf(stderr,
            "[sintra] watchdog '%s' triggered after %lld seconds – forcing stack dump\n",
            label.c_str(),
            static_cast<long long>(timeout.count()));
        std::fflush(stderr);

        pthread_kill(thread_id, SIGUSR2);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::abort();
    }).detach();
#else
    (void)timeout;
    (void)context;
#endif
}

} // namespace sintra::detail

#endif // SINTRA_DETAIL_DEBUG_WATCHDOG_H
