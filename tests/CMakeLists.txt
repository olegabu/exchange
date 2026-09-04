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
