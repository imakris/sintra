add_executable(sintra_broadcast_benchmark broadcast_benchmark.cpp harness.h)
target_link_libraries(sintra_broadcast_benchmark PRIVATE sintra sintra_dev_flags)
# The build facts record the configuration; only a Release build is valid.
target_compile_definitions(sintra_broadcast_benchmark PRIVATE
    SINTRA_BENCHMARK_CONFIG="$<CONFIG>")

add_executable(sintra_rpc_benchmark rpc_benchmark.cpp harness.h)
target_link_libraries(sintra_rpc_benchmark PRIVATE sintra sintra_dev_flags)
target_compile_definitions(sintra_rpc_benchmark PRIVATE
    SINTRA_BENCHMARK_CONFIG="$<CONFIG>")

add_executable(sintra_swarm_benchmark swarm_benchmark.cpp harness.h)
target_link_libraries(sintra_swarm_benchmark PRIVATE sintra sintra_dev_flags)
target_compile_definitions(sintra_swarm_benchmark PRIVATE
    SINTRA_BENCHMARK_CONFIG="$<CONFIG>")
