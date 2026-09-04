# docs/spec.md §9.1: the apply path under google benchmark, before any
# fleet time is spent. `make bench` runs it in the release preset.
add_executable(apply_benchmark apply_benchmark.cpp)
target_include_directories(apply_benchmark PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(apply_benchmark PRIVATE exchange::state_machine sequencer::common benchmark::benchmark benchmark::benchmark_main)

# docs/spec.md §9.1 step 6: the FIX order-entry load generator. Forked
# from sequencer's FixRequester -- see exchange_fix_requester.hpp for
# why -- so it links bench_load_generator (the harness) and fix_session
# (the session core), not bench_fix_requester.
add_executable(exchange_load_generator_main exchange_load_generator_main.cpp)
set_target_properties(exchange_load_generator_main PROPERTIES OUTPUT_NAME exchange_load_generator)
target_include_directories(exchange_load_generator_main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(exchange_load_generator_main PRIVATE
  sequencer::bench_load_generator sequencer::fix_session
  gflags::gflags_static glog::glog sequencer::common)

# The output codec's cost per journal record: a hard ceiling on records
# per second, because it runs on the output gateway's single
# journal-tail thread (docs/spec.md §9.1, measurements.md §3).
add_executable(output_codec_benchmark output_codec_benchmark.cpp)
target_link_libraries(output_codec_benchmark PRIVATE
  exchange::fix_output_codec benchmark::benchmark benchmark::benchmark_main)
