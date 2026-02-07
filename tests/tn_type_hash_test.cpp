#include <sintra/sintra.h>

#include "test_utils.h"

#include <cstdio>
#include <string>
#include <unordered_map>

namespace {

bool run_tn_type_hash_test()
{
    sintra::tn_type a{42, "alpha"};
    sintra::tn_type b{42, "alpha"};
    sintra::tn_type c{7, "beta"};

    std::unordered_map<sintra::tn_type, int> map;
    map.emplace(a, 1);
    map.emplace(c, 2);

    const auto it = map.find(b);
    if (it == map.end()) {
        std::fprintf(stderr, "FAIL: lookup for matching tn_type failed\n");
        return false;
    }
    if (it->second != 1) {
        std::fprintf(stderr, "FAIL: expected value 1, got %d\n", it->second);
        return false;
    }

    return true;
}

} // namespace

int main()
{
    const bool ok = run_tn_type_hash_test();
    return ok ? 0 : 1;
}
