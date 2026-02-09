// Shared helpers for choreography-style tests.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sintra::test {

inline std::string make_barrier_name(std::string_view prefix, std::uint64_t a)
{
    std::ostringstream oss;
    oss << prefix << '-' << a;
    return oss.str();
}

inline std::string make_barrier_name(std::string_view prefix,
                                     std::uint64_t a,
                                     std::uint64_t b)
{
    std::ostringstream oss;
    oss << prefix << '-' << a << '-' << b;
    return oss.str();
}

inline std::string make_barrier_name(std::string_view prefix,
                                     std::uint64_t a,
                                     std::string_view mid,
                                     std::uint64_t b)
{
    std::ostringstream oss;
    oss << prefix << '-' << a << '-' << mid << '-' << b;
    return oss.str();
}

inline std::string make_barrier_name(std::string_view prefix,
                                     std::uint64_t a,
                                     std::string_view suffix)
{
    std::ostringstream oss;
    oss << prefix << '-' << a << '-' << suffix;
    return oss.str();
}

inline std::string make_barrier_name(std::string_view prefix,
                                     std::uint64_t a,
                                     std::uint64_t b,
                                     std::string_view suffix)
{
    std::ostringstream oss;
    oss << prefix << '-' << a << '-' << b << '-' << suffix;
    return oss.str();
}

struct Choreography_result
{
    bool ok = false;
    std::vector<std::string> lines;
};

inline void write_choreography_result(const std::filesystem::path& file,
                                      bool ok,
                                      const std::vector<std::string>& lines)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    out << (ok ? "ok" : "fail") << '\n';
    for (const auto& line : lines) {
        out << line << '\n';
    }
}

inline Choreography_result read_choreography_result(const std::filesystem::path& file)
{
    Choreography_result result;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return result;
    }

    std::string line;
    if (!std::getline(in, line)) {
        return result;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    result.ok = (line == "ok");

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        result.lines.push_back(std::move(line));
    }

    return result;
}

struct Phase_round
{
    int phase = 0;
    int round = 0;
    std::uint64_t token = 0;
    std::string pre_barrier;
    std::string post_barrier;
};

template <typename Plan,
          std::size_t N,
          typename Token_fn,
          typename Callback,
          typename Stop_fn>
bool for_each_phase_round(const std::array<Plan, N>& plan,
                          std::string_view prefix,
                          std::string_view pre_label,
                          std::string_view post_label,
                          Token_fn&& token_fn,
                          Callback&& callback,
                          Stop_fn&& stop_fn)
{
    for (const auto& phase_plan : plan) {
        for (int round = 0; round < phase_plan.rounds; ++round) {
            if (stop_fn()) {
                return false;
            }
            Phase_round ctx;
            ctx.phase = phase_plan.phase;
            ctx.round = round;
            ctx.token = token_fn(ctx.phase, ctx.round);
            ctx.pre_barrier = make_barrier_name(prefix, ctx.phase, ctx.round, pre_label);
            ctx.post_barrier = make_barrier_name(prefix, ctx.phase, ctx.round, post_label);
            if (!callback(ctx)) {
                return false;
            }
        }
    }
    return true;
}

template <typename Plan,
          std::size_t N,
          typename Token_fn,
          typename Callback>
bool for_each_phase_round(const std::array<Plan, N>& plan,
                          std::string_view prefix,
                          std::string_view pre_label,
                          std::string_view post_label,
                          Token_fn&& token_fn,
                          Callback&& callback)
{
    return for_each_phase_round(plan,
                                prefix,
                                pre_label,
                                post_label,
                                std::forward<Token_fn>(token_fn),
                                std::forward<Callback>(callback),
                                [] { return false; });
}

} // namespace sintra::test
