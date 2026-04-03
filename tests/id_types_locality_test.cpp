#include <sintra/detail/globals.h>
#include <sintra/detail/id_types.h>

#include "test_utils.h"

namespace {

constexpr std::string_view k_failure_prefix = "id_types_locality_test failure: ";

void expect_locality(sintra::instance_id_type instance_id, bool expected, const char* label)
{
    const bool actual = sintra::is_local(instance_id);
    sintra::test::expect(
        actual == expected,
        k_failure_prefix,
        std::string(label) + ": expected is_local=" + (expected ? "true" : "false"));
}

void run_cases_for_local_process(std::uint32_t local_process_index)
{
    sintra::s_mproc_id = sintra::make_process_instance_id(local_process_index);

    const auto local_instance = sintra::compose_instance(local_process_index, 7);
    const auto remote_instance = sintra::compose_instance(local_process_index + 1, 7);
    const auto all_except_process_2 =
        sintra::compose_instance(static_cast<std::uint32_t>(~std::uint32_t{2}), 7);

    expect_locality(local_instance, true, "explicit local process");
    expect_locality(remote_instance, false, "explicit remote process");
    expect_locality(sintra::any_remote, false, "any_remote");
    expect_locality(sintra::any_local_or_remote, true, "any_local_or_remote");
    expect_locality(
        all_except_process_2,
        local_process_index != 2,
        "complemented process selection");
}

} // namespace

int main()
{
    const auto saved_mproc_id = sintra::s_mproc_id;

    run_cases_for_local_process(2);
    run_cases_for_local_process(3);

    sintra::s_mproc_id = saved_mproc_id;
    return 0;
}
