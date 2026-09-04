# liquibook is header-only (VENDORED.md). One INTERFACE target, SYSTEM
# so that our -Werror stays ours: upstream's warnings are upstream's.
add_library(exchange_liquibook INTERFACE)
add_library(exchange::liquibook ALIAS exchange_liquibook)
target_include_directories(exchange_liquibook SYSTEM INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_features(exchange_liquibook INTERFACE cxx_std_11)
