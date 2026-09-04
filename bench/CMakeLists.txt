# docs/spec.md §9.1: the apply path under google benchmark, before any
# fleet time is spent. `make bench` runs it in the release preset.
add_executable(apply_benchmark apply_benchmark.cpp)
target_include_directories(apply_benchmark PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(apply_benchmark PRIVATE exchange::state_machine benchmark::benchmark benchmark::benchmark_main)
