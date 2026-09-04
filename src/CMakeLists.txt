# One library per layer (docs/spec.md §8). The book layer has no
# sequencer dependency by construction; the state machine composes it.

# --- state machine -----------------------------------------------------
add_library(exchange_state_machine STATIC
  state_machine/order_book_state_machine.cpp
)
add_library(exchange::state_machine ALIAS exchange_state_machine)
target_include_directories(exchange_state_machine PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
target_link_libraries(exchange_state_machine PUBLIC sequencer::node exchange::liquibook)
target_compile_features(exchange_state_machine PUBLIC cxx_std_20)

# --- executables -------------------------------------------------------
add_executable(exchange_node_main main/exchange_node_main.cpp)
set_target_properties(exchange_node_main PROPERTIES OUTPUT_NAME exchange_node)
target_link_libraries(exchange_node_main PRIVATE exchange::state_machine sequencer::node)
