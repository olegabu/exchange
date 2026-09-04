# One library per layer (docs/spec.md §8). The book layer has no
# sequencer dependency by construction; the state machine composes it.

# --- book: the matcher. liquibook and the standard library only. -------
add_library(exchange_book STATIC book/book.cpp)
add_library(exchange::book ALIAS exchange_book)
target_include_directories(exchange_book PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
target_link_libraries(exchange_book PUBLIC exchange::liquibook)
target_compile_features(exchange_book PUBLIC cxx_std_20)

# --- state machine -----------------------------------------------------
add_library(exchange_state_machine STATIC
  state_machine/order_book_state_machine.cpp
)
add_library(exchange::state_machine ALIAS exchange_state_machine)
target_include_directories(exchange_state_machine PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
target_link_libraries(exchange_state_machine PUBLIC sequencer::journal exchange::book exchange::sbe)
target_compile_features(exchange_state_machine PUBLIC cxx_std_20)

# --- executables -------------------------------------------------------
add_executable(exchange_node_main main/exchange_node_main.cpp)
set_target_properties(exchange_node_main PROPERTIES OUTPUT_NAME exchange_node)
target_link_libraries(exchange_node_main PRIVATE exchange::state_machine sequencer::node)

add_executable(exchange_replay_main main/exchange_replay_main.cpp)
set_target_properties(exchange_replay_main PROPERTIES OUTPUT_NAME exchange_replay)
target_link_libraries(exchange_replay_main PRIVATE exchange::state_machine sequencer::replay)

# --- FIX codecs: one library per direction (design.md §2) ----------------
add_library(exchange_fix_input_codec STATIC fix/fix_input_codec.cpp)
add_library(exchange::fix_input_codec ALIAS exchange_fix_input_codec)
target_include_directories(exchange_fix_input_codec PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
# fix_session only for hffix's include directory.
target_link_libraries(exchange_fix_input_codec PUBLIC
  sequencer::input_codec_api sequencer::fix_session exchange::sbe)
target_compile_features(exchange_fix_input_codec PUBLIC cxx_std_20)

add_library(exchange_fix_output_codec STATIC fix/fix_output_codec.cpp)
add_library(exchange::fix_output_codec ALIAS exchange_fix_output_codec)
target_include_directories(exchange_fix_output_codec PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
target_link_libraries(exchange_fix_output_codec PUBLIC sequencer::output_codec_api exchange::sbe)
target_compile_features(exchange_fix_output_codec PUBLIC cxx_std_20)

add_executable(exchange_fix_gateway_main main/exchange_fix_gateway_main.cpp)
set_target_properties(exchange_fix_gateway_main PROPERTIES OUTPUT_NAME exchange_fix_gateway)
target_link_libraries(exchange_fix_gateway_main PRIVATE
  exchange::fix_input_codec exchange::fix_output_codec sequencer::fix_gateway
  gflags::gflags_static glog::glog sequencer::common)

add_executable(exchange_admin_main main/exchange_admin_main.cpp)
set_target_properties(exchange_admin_main PROPERTIES OUTPUT_NAME exchange_admin)
target_link_libraries(exchange_admin_main PRIVATE
  exchange::sbe sequencer::node gflags::gflags_static glog::glog sequencer::common)
target_include_directories(exchange_admin_main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
