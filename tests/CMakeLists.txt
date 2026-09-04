find_package(Threads REQUIRED)
include(GoogleTest)

# Step 1: the state machine's placeholder contract. Real suites arrive
# with each build step (docs/spec.md §8).
add_executable(exchange_state_machine_test state_machine_test.cpp)
target_link_libraries(exchange_state_machine_test PRIVATE
  exchange::state_machine GTest::gtest GTest::gtest_main sequencer::common)
gtest_discover_tests(exchange_state_machine_test)

# Step 2: the wire format -- round trips, and the frozen layout (spec §4).
add_executable(exchange_schema_test schema_test.cpp)
target_link_libraries(exchange_schema_test PRIVATE
  exchange::state_machine GTest::gtest GTest::gtest_main)
gtest_discover_tests(exchange_schema_test)

# Step 3: matching semantics and the two determinism gates (spec §5).
add_executable(exchange_matching_test matching_test.cpp)
target_link_libraries(exchange_matching_test PRIVATE
  exchange::state_machine GTest::gtest GTest::gtest_main)
gtest_discover_tests(exchange_matching_test)

add_executable(exchange_determinism_test determinism_test.cpp)
target_link_libraries(exchange_determinism_test PRIVATE
  exchange::state_machine sequencer::replay GTest::gtest GTest::gtest_main sequencer::common)
gtest_discover_tests(exchange_determinism_test PROPERTIES TIMEOUT 300)

# Step 4: snapshots (spec §6).
add_executable(exchange_snapshot_test snapshot_test.cpp)
target_link_libraries(exchange_snapshot_test PRIVATE
  exchange::state_machine GTest::gtest GTest::gtest_main sequencer::common)
gtest_discover_tests(exchange_snapshot_test PROPERTIES TIMEOUT 300)

# Step 5: the FIX codecs, and the whole thing as real processes (spec §7).
add_executable(exchange_fix_codec_test fix_codec_test.cpp)
target_link_libraries(exchange_fix_codec_test PRIVATE
  exchange::fix_input_codec exchange::fix_output_codec exchange::state_machine
  GTest::gtest GTest::gtest_main)
gtest_discover_tests(exchange_fix_codec_test)

add_executable(exchange_fix_end_to_end_test fix_end_to_end_test.cpp)
target_link_libraries(exchange_fix_end_to_end_test PRIVATE
  sequencer::fix_session GTest::gtest GTest::gtest_main Threads::Threads sequencer::common)
target_compile_definitions(exchange_fix_end_to_end_test PRIVATE
  EXCHANGE_NODE_PATH="$<TARGET_FILE:exchange_node_main>"
  EXCHANGE_FIX_GATEWAY_PATH="$<TARGET_FILE:exchange_fix_gateway_main>"
  EXCHANGE_ADMIN_PATH="$<TARGET_FILE:exchange_admin_main>"
)
add_dependencies(exchange_fix_end_to_end_test exchange_node_main exchange_fix_gateway_main exchange_admin_main)
# Labelled so the tsan preset can skip it, and ONLY it. The test spawns
# exchange_node and exchange_fix_gateway, which link brpc; brpc's
# bthread is an M:N scheduler that switches stacks under the runtime,
# which ThreadSanitizer's happens-before model cannot follow, so it
# reports races inside brpc/braft on every run. Verified pre-existing
# rather than assumed: sequencer's own node_integration_test under its
# tsan preset reports the same eight races in the same brpc internals
# (docs/design.md §4). Nothing in this repository is multi-threaded --
# the state machine, the book and both codecs are called from one
# thread -- so tsan's value here is the other 39 tests, which run.
gtest_discover_tests(exchange_fix_end_to_end_test PROPERTIES TIMEOUT 180 LABELS "spawns-brpc")

# Step 6: what the load generator's flow actually does (spec §9.1).
add_executable(exchange_load_generator_shape_test load_generator_shape_test.cpp)
target_include_directories(exchange_load_generator_shape_test PRIVATE "${CMAKE_SOURCE_DIR}/bench")
target_link_libraries(exchange_load_generator_shape_test PRIVATE
  exchange::state_machine sequencer::bench_load_generator sequencer::fix_session
  GTest::gtest GTest::gtest_main)
gtest_discover_tests(exchange_load_generator_shape_test PROPERTIES TIMEOUT 120)
