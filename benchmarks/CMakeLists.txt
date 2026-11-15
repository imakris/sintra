add_executable(sintra_broadcast_benchmark broadcast_benchmark.cpp harness.h)
target_link_libraries(sintra_broadcast_benchmark PRIVATE sintra sintra_dev_flags)
# The build facts record the configuration; only a Release build is valid.
target_compile_definitions(sintra_broadcast_benchmark PRIVATE
    SINTRA_BENCHMARK_CONFIG="$<CONFIG>")
